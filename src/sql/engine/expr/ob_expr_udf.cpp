/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "ob_expr_udf.h"
#include "observer/ob_server_struct.h"
#include "observer/ob_server.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_spi.h"
#include "pl/ob_pl.h"
#include "pl/ob_pl_stmt.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObExprUDF, ObFuncExprOperator),
                     udf_id_, result_type_, params_type_,
                     udf_package_id_, params_desc_, is_udt_udf_,
                     nocopy_params_, subprogram_path_, call_in_sql_, loc_, is_udt_cons_);

ObExprUDF::ObExprUDF(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_UDF, N_UDF, PARAM_NUM_UNKNOWN, NOT_ROW_DIMENSION,
                         INTERNAL_IN_MYSQL_MODE, INTERNAL_IN_ORACLE_MODE),
      udf_id_(OB_INVALID_ID),
      udf_package_id_(OB_INVALID_ID),
      subprogram_path_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc)),
      result_type_(),
      params_type_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc)),
      params_desc_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc)),
      nocopy_params_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(alloc)),
      is_udt_udf_(false),
      call_in_sql_(true),
      loc_(0),
      is_udt_cons_(false) {}

ObExprUDF::~ObExprUDF() {}

void ObExprUDF::reset()
{
  udf_id_ = common::OB_INVALID_ID;
  udf_package_id_ = common::OB_INVALID_ID;
  params_type_.reset();
  params_desc_.reset();
  is_udt_udf_ = false;
  call_in_sql_ = true;
  nocopy_params_.reset();
  loc_ = 0;
  is_udt_cons_ = false;
  ObFuncExprOperator::reset();
}

int ObExprUDF::assign(const ObExprOperator &other)
{
  int ret = OB_SUCCESS;
  const ObExprUDF *tmp_other = dynamic_cast<const ObExprUDF*>(&other);
  if (OB_UNLIKELY(OB_ISNULL(tmp_other))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("cast failed, type of argument is wrong", K(ret), K(other));
  } else if (OB_FAIL(subprogram_path_.assign(tmp_other->get_subprogram_path()))) {
    LOG_WARN("failed to assign subprogram path", K(ret));
  } else if (OB_FAIL(params_type_.assign(tmp_other->get_params_type()))) {
    LOG_WARN("failed to assign params type", K(ret));
  } else if (OB_FAIL(nocopy_params_.assign(tmp_other->get_nocopy_params()))) {
    LOG_WARN("failed to assign nocopy params", K(ret));
  } else if (OB_FAIL(result_type_.assign(tmp_other->get_result_type()))) {
    LOG_WARN("failed to assign result type", K(ret));
  } else if (OB_FAIL(params_desc_.assign(tmp_other->get_params_desc()))) {
    LOG_WARN("failed to assign params desc", K(ret));
  } else {
    udf_id_ = tmp_other->get_udf_id();
    udf_package_id_ = tmp_other->get_udf_package_id();
    is_udt_udf_ = tmp_other->get_is_udt_udf();
    if (OB_FAIL(ObExprOperator::assign(other))) {
      LOG_WARN("failed to ObExprOperator::assign", K(ret));
    } else {/*do nothing*/}
  }
  return ret;
}

int ObExprUDF::calc_result_typeN(ObExprResType &type,
                                 ObExprResType *types,
                                 int64_t param_num,
                                 ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  CK(param_num == params_type_.count());
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < param_num; i++) {
      if (params_desc_.at(i).is_out()
          && params_desc_.at(i).is_obj_access_out()) {
        ObObjMeta meta;
        meta.set_ext();
        types[i].set_calc_meta(meta);
      } else {
        types[i].set_calc_accuracy(params_type_.at(i).get_accuracy());
        types[i].set_calc_meta(params_type_.at(i).get_obj_meta());
        if (params_type_.at(i).get_collation_type() == CS_TYPE_ANY) {
          if (types[i].is_string_or_lob_locator_type()) {
            types[i].set_calc_collation_type(types[i].get_collation_type());
          } else {
            types[i].set_calc_collation_type(type_ctx.get_session()->get_nls_collation());
          }
        }
      }
    }
    type.set_accuracy(result_type_.get_accuracy());
    type.set_meta(result_type_.get_obj_meta());
    if (type.get_type() == ObRawType) {
      type.set_collation_level(CS_LEVEL_NUMERIC);
    }
    if (!type.is_ext()) {
      if (lib::is_oracle_mode()) {
        type.set_length(OB_MAX_ORACLE_VARCHAR_LENGTH);
      } else {
        type.set_length(OB_MAX_VARCHAR_LENGTH);
      }
    }
  }
  return ret;
}

int ObExprUDF::check_types(ObObj &result, const ObObj *objs_stack, int64_t param_num) const
{
  int ret = OB_SUCCESS;
  CK (OB_NOT_NULL(objs_stack));
  CK (param_num == params_type_.count());
  CK (param_num == params_desc_.count());
  UNUSED(result);
  for (int64_t i = 0; OB_SUCC(ret) && i < param_num; i++) {
    if (!objs_stack[i].is_null()
        && (!params_desc_.at(i).is_out())) {
      TYPE_CHECK(objs_stack[i], params_type_.at(i).get_type());
    }
  }
  return ret;
}

int ObExprUDF::check_types(const ObExpr &expr, const ObExprUDFInfo &info)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    if (!expr.args_[i]->obj_meta_.is_null()
        && (!info.params_desc_.at(i).is_out())) {
      if (expr.args_[i]->obj_meta_.get_type() != info.params_type_.at(i).get_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("check param type failed", K(ret), K(i));
      }
    }
  }
  return ret;
}

bool ObExprUDF::need_deep_copy_in_parameter(const ObObj *objs_stack,
                                            int64_t param_num,
                                            const ObIArray<ObUDFParamDesc> &params_desc,
                                            const ObIArray<ObExprResType> &params_type,
                                            const ObObj &element)
{
  bool result = false;
  for (int64_t i = 0; !result && i < param_num; ++i) {
    if (params_desc.at(i).is_out()
        && ObExtendType == params_type.at(i).get_type()
        && element.get_ext() == objs_stack[i].get_ext()
        && objs_stack[i].is_pl_extend()
        && element.is_pl_extend()
        && objs_stack[i].get_meta().get_extend_type() == element.get_meta().get_extend_type()) {
      result = true;
    }
  }
  return result;
}

int ObExprUDF::process_in_params(const ObObj *objs_stack,
                                 int64_t param_num,
                                 const ObIArray<ObUDFParamDesc> &params_desc,
                                 const ObIArray<ObExprResType> &params_type,
                                 ParamStore& iparams,
                                 ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  CK (0 == param_num || OB_NOT_NULL(objs_stack));
  CK (param_num == params_desc.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < param_num; ++i) {
    ObObjParam param;
    param.reset();
    if (params_type.at(i).is_null()) {
      // default value, mock a max obj to tell pl engine here need replace to default value.
      param.set_is_pl_mock_default_param(true);
    } else if (!params_desc.at(i).is_out()) { // in parameter
      if (ObExtendType == params_type.at(i).get_type()) {
        if (need_deep_copy_in_parameter(
            objs_stack, param_num, params_desc, params_type, objs_stack[i])) {
          OZ (pl::ObUserDefinedType::deep_copy_obj(allocator, objs_stack[i], param, true));
        } else {
          param.set_extend(objs_stack[i].get_ext(),
                         objs_stack[i].get_meta().get_extend_type(), objs_stack[i].get_val_len());
          param.set_param_meta();
        }
      } else {
        objs_stack[i].copy_value_or_obj(param, true);
      }
    } else if (params_desc.at(i).is_local_out()
              || params_desc.at(i).is_package_var_out()
              || params_desc.at(i).is_subprogram_var_out()) {
      objs_stack[i].copy_value_or_obj(param, true);
    } else {
      if (params_type.at(i).get_type() == ObExtendType) {
        param.set_extend(objs_stack[i].get_ext(),
                         objs_stack[i].get_meta().get_extend_type(), objs_stack[i].get_val_len());
        param.set_param_meta();
      } else {
        void *ptr = NULL;
        ObObj *obj = NULL;
        CK (objs_stack[i].is_ext());
        OX (ptr = reinterpret_cast<void*>(objs_stack[i].get_ext()));
        CK (OB_NOT_NULL(ptr));
        OX (obj = reinterpret_cast<ObObj*>(ptr));
        CK (OB_NOT_NULL(obj));
        OX ((*obj).copy_value_or_obj(param, true));
        OX (param.set_param_meta());
      }
    }
    if (OB_SUCC(ret) && params_type.at(i).get_type() == ObExtendType) {
      param.set_udt_id(params_type.at(i).get_udt_id());
    }
    OZ (iparams.push_back(param));
  }
  return ret;
}

int ObExprUDF::process_out_params(const ObObj *objs_stack,
                                  int64_t param_num,
                                  ParamStore& iparams,
                                  ObIAllocator &alloc,
                                  ObExecContext &exec_ctx,
                                  const ObIArray<int64_t> &nocopy_params,
                                  const ObIArray<ObUDFParamDesc> &params_desc,
                                  const ObIArray<ObExprResType> &params_type)
{
  int ret = OB_SUCCESS;
  UNUSED (param_num);
  CK (iparams.count() == params_desc.count());
  CK (0 == nocopy_params.count() || nocopy_params.count() == iparams.count());
  // 先处理NoCopy参数
  for (int64_t i = 0; OB_SUCC(ret) && i < iparams.count(); ++i) {
    if (!params_desc.at(i).is_out()) {
      // do nothing ...
    } else if (params_desc.at(i).is_local_out() && nocopy_params.at(i) != OB_INVALID_INDEX) {
      const ParamStore &param_store = exec_ctx.get_physical_plan_ctx()->get_param_store();
      int64_t position = params_desc.at(i).get_index();
      ObObjParam *modify = NULL;
      ObObjParam result;
      CK (position < param_store.count());
      CK (OB_NOT_NULL(modify = const_cast<ObObjParam*>(&(param_store.at(position)))));
      OZ (sql::ObSPIService::spi_convert(exec_ctx.get_my_session(),
                                         &alloc,
                                         iparams.at(i),
                                         params_type.at(i),
                                         result));
      OX (result.copy_value_or_obj(*modify, true));
      OX (modify->set_param_meta());
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < iparams.count(); ++i) {
    if (!params_desc.at(i).is_out()) {
      // do nothing ...
    } else if (params_desc.at(i).is_local_out()) { //out param in paramstore of caller
      if (nocopy_params.count() > 0 && nocopy_params.at(i) != OB_INVALID_INDEX) {
        // nocopy parameter already process before, do nothing ....
      } else {
        const ParamStore &param_store = exec_ctx.get_physical_plan_ctx()->get_param_store();
        int64_t position = params_desc.at(i).get_index();
        ObObjParam *modify = NULL;
        ObObjParam result;
        CK (position < param_store.count());
        CK (OB_NOT_NULL(modify = const_cast<ObObjParam*>(&(param_store.at(position)))));
        // ext type cannot convert. just copy it.
        if (iparams.at(i).is_ext()) {
          // caller param may ref cursor, which may not allocated.
          if (modify->is_null()) {
            OX (iparams.at(i).copy_value_or_obj(*modify, true));
            if (iparams.at(i).is_ref_cursor_type()) {		
              modify->set_is_ref_cursor_type(true);		
            }
            OX (modify->set_param_meta());
          } else if (!modify->is_ext()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("process function out param failed, type mismatch", K(ret),
                                                                   K(iparams.at(i)), K(*modify));
          } else {
            OX (iparams.at(i).copy_value_or_obj(*modify, true));
            OX (modify->set_param_meta());
          }
        } else {
          OZ (sql::ObSPIService::spi_convert(exec_ctx.get_my_session(),
                                             &alloc,
                                             iparams.at(i),
                                             params_type.at(i),
                                             result));
          OX (result.copy_value_or_obj(*modify, true));
          OX (modify->set_param_meta());
        }
      }
    } else if (params_desc.at(i).is_package_var_out()) {
      OZ (ObSPIService::spi_set_package_variable(
        &exec_ctx,
        NULL,
        params_desc.at(i).get_package_id(),
        params_desc.at(i).get_index(),
        iparams.at(i)));
    } else if (params_desc.at(i).is_subprogram_var_out()) {
      OZ (pl::ObPLContext::set_subprogram_var_from_local(
        *exec_ctx.get_my_session(),
        params_desc.at(i).get_package_id(),
        params_desc.at(i).get_subprogram_id(),
        params_desc.at(i).get_index(),
        iparams.at(i)));
    } else if (!params_type.at(i).is_ext()) {
      void *ptr = NULL;
      ObObj *obj = NULL;
      ObObjParam result;
      CK (objs_stack[i].is_ext());
      OX (ptr = reinterpret_cast<void*>(objs_stack[i].get_ext()));
      CK (OB_NOT_NULL(ptr));
      OX (obj = reinterpret_cast<ObObj*>(ptr));
      CK (OB_NOT_NULL(obj));
      OZ (sql::ObSPIService::spi_convert(exec_ctx.get_my_session(),
                                         &alloc, iparams.at(i),
                                         params_type.at(i),
                                         result));
      OX (result.copy_value_or_obj(*obj, true));
      OX (result.set_param_meta());
    } else {
      // do nothing ...
    }
  }
  return ret;
}

int ObExprUDF::cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr, ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  ObIAllocator &alloc = *expr_cg_ctx.allocator_;
  const ObUDFRawExpr &fun_sys = static_cast<const ObUDFRawExpr &>(raw_expr);
  ObExprUDFInfo *info = OB_NEWx(ObExprUDFInfo, (&alloc), alloc, T_FUN_UDF);
  if (NULL == info) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret));
  } else {
    OZ(info->from_raw_expr(fun_sys));
    info->is_called_in_sql_ = is_called_in_sql();
    rt_expr.extra_info_ = info;
  }
  rt_expr.eval_func_ = eval_udf;
  return ret;
}

int ObExprUDF::ObExprUDFCtx::init_param_store(ObIAllocator &allocator,
                                              int param_num)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(param_store_buf_ = allocator.alloc(sizeof(ParamStore)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate memory", K(ret));
  } else {
    params_ = new(param_store_buf_)ParamStore(ObWrapperAllocator(allocator));
  }
  OZ (params_->prepare_allocate(param_num));
  OX (params_->reuse());
  return ret;
}

int ObExprUDF::build_udf_ctx(int64_t udf_ctx_id,
                             int64_t param_num,
                             ObExecContext &exec_ctx,
                             ObExprUDFCtx *&udf_ctx)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(udf_ctx = static_cast<ObExprUDFCtx *>(exec_ctx.get_expr_op_ctx(udf_ctx_id)))) {
    if (OB_FAIL(exec_ctx.create_expr_op_ctx(udf_ctx_id, udf_ctx))) {
      LOG_WARN("failed to create operator ctx", K(ret));
    } else if (OB_FAIL(udf_ctx->init_param_store(exec_ctx.get_allocator(), param_num))) {
      LOG_WARN("failed to init param", K(ret));
    }
  } else {
    OX (udf_ctx->reuse());
  }
  return ret;
}

int ObExprUDF::eval_udf(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObObj tmp_result;
  ObObj result;
  pl::ObPL *pl_engine = nullptr;
  ParamStore *udf_params = nullptr;
  uint64_t udf_ctx_id = static_cast<uint64_t>(expr.expr_ctx_id_);
  ObExprUDFCtx *udf_ctx = nullptr;
  share::schema::ObSchemaGetterGuard schema_guard;
  ObSQLSessionInfo *session = nullptr;
  ObIAllocator &alloc = ctx.exec_ctx_.get_allocator();
  const ObExprUDFInfo *info = static_cast<ObExprUDFInfo *>(expr.extra_info_);
  ObObj *objs = nullptr;
  CK(0 == expr.arg_cnt_ || OB_NOT_NULL(objs = static_cast<ObObj *> (alloc.alloc(expr.arg_cnt_ * sizeof(ObObj)))));
  CK(OB_NOT_NULL(info));
  bool is_stack_overflow = false;
  OZ(check_stack_overflow(is_stack_overflow));
  if (OB_SUCC(ret) && is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive",
             K(ret), K(is_stack_overflow), K(info->udf_package_id_), K(info->udf_id_));
  }
  if (OB_SUCC(ret) && expr.arg_cnt_ != info->params_desc_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("udf parameter number is not equel to params desc count",
             K(ret), K(expr.arg_cnt_), K(info->params_desc_.count()), K(info->params_desc_));
  }
  CK(OB_NOT_NULL(session = ctx.exec_ctx_.get_my_session()));
  CK(OB_NOT_NULL(pl_engine = session->get_pl_engine()));
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr.eval_param_value(ctx))) {
    LOG_WARN("failed o eval param value", K(ret));
  } else if (OB_FAIL(build_udf_ctx(udf_ctx_id, expr.arg_cnt_, ctx.exec_ctx_, udf_ctx))) {
    LOG_WARN("failed to build_udf_ctx", K(ret), K(udf_ctx_id));
  } else if (OB_ISNULL(udf_params = udf_ctx->get_param_store())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate memory", K(ret));
  }
  OZ (check_types(expr, *info));
  OZ (fill_obj_stack(expr, ctx, objs));
  OZ (process_in_params(
    objs, expr.arg_cnt_, info->params_desc_, info->params_type_, *udf_params, alloc));
  // replace first param when is udt constructor, 
  /* for example:
  * a := demo(3,4) will be rewirte to a :=demo(null, 3, 4) after resolve
  * here will have to change to a := demo(self, 3, 4), which self is a object type
  * who's type id is udf_package_id_
  */
  if (OB_FAIL(ret)) {
    // do nothing ...
  } else if (!lib::is_oracle_mode() && info->is_udt_cons_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected, udt constructor must be oracle mode", K(ret));
  } else {
    bool need_end_stmt = false;
    //由于这是临时处理，用中文注释解释，便于理解
    //根据和Oracle的兼容性，我们认为select pl_udf() from dual也是一种嵌套语句
    //这里以前认为这种情况不属于嵌套语句，导致跟Oracle的行为存在不兼容
    //由于begin_nested_session等相关的旧接口还没有去掉
    //因此在这里特殊处理一下，对这种情况下,在UDF执行前调用session->set_start_stmt()
    //目的是骗过nested session的一些接口的参数检查，等后续完全去掉nested session相关的接口
    //这里的特殊处理也会去掉
    stmt::StmtType parent_stmt = ctx.exec_ctx_.get_sql_ctx()->stmt_type_;
    if (!session->has_start_stmt() && stmt::StmtType::T_SELECT == parent_stmt) {
      need_end_stmt = true;
      session->set_start_stmt();
    }
    if (info->is_udt_cons_) {
      pl::ObPLUDTNS ns(*ctx.exec_ctx_.get_sql_ctx()->schema_guard_);
      pl::ObPLDataType pl_type;
      pl_type.set_user_type_id(pl::PL_RECORD_TYPE, info->udf_package_id_);
      pl_type.set_type_from(pl::PL_TYPE_UDT);
      CK (0 < udf_params->count());
      OZ (ns.init_complex_obj(alloc, pl_type, udf_params->at(0), false, false));
    }
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    try {
      int64_t package_id = info->is_udt_udf_ ?
           share::schema::ObUDTObjectType::mask_object_id(info->udf_package_id_)
           : info->udf_package_id_;
      OZ(pl_engine->execute(ctx.exec_ctx_,
                            info->is_called_in_sql_ ? tmp_alloc_g.get_allocator()
                                                    : alloc,
                            package_id,
                            info->udf_id_,
                            info->subprogram_path_,
                            *udf_params,
                            info->nocopy_params_,
                            tmp_result,
                            nullptr,
                            false,
                            true,
                            info->loc_,
                            info->is_called_in_sql_),

                            info->udf_id_,
                            info->udf_package_id_,
                            info->is_udt_udf_,
                            tmp_result,
                            package_id);
    } catch(...) {
//      OZ(after_calc_result(schema_guard, sql_ctx, ctx.exec_ctx_));
      throw;
    }
    if (OB_SUCC(ret)) {
      if (tmp_result.is_pl_extend()) {
        OZ (pl::ObUserDefinedType::deep_copy_obj(alloc, tmp_result, result, true));
      } else {
        OZ (deep_copy_obj(alloc, tmp_result, result));
      }
      if (info->is_called_in_sql_) {
        OX (ctx.exec_ctx_.get_pl_ctx()->reset_obj());
      }
    }
    if (OB_SUCC(ret) && info->is_udt_cons_) {
      pl::ObPLComposite *obj_self = reinterpret_cast<pl::ObPLRecord *>(udf_params->at(0).get_ext());
      CK (OB_NOT_NULL(obj_self));
      if (OB_SUCC(ret) && obj_self->is_record()) {
        OX (obj_self->set_is_null(false));
      }
    }
    //兼容oracle udf调用，如果udf是在sql(或pl的sql)中调用，则将错误码OB_READ_NOTHING覆盖为OB_SUCCESS
    //函数返回结果为null，否则不对错误码进行覆盖
    if (OB_READ_NOTHING == ret && info->is_called_in_sql_ && lib::is_oracle_mode()) {
      ret = OB_SUCCESS;
    }
    if (OB_SUCC(ret) && info->is_called_in_sql_ && lib::is_oracle_mode()) {
      // UDF 和系统包函数里支持出现最大长度为 32767 byte 的 raw 类型变量。
      // 但如果 UDF 和系统包函数被 SQL 调用且返回值类型是 raw，则长度不允许超过 2000 byte。
      if (result.is_raw() && result.get_raw().length() > OB_MAX_ORACLE_RAW_SQL_COL_LENGTH) {
        ret = OB_ERR_NUMERIC_OR_VALUE_ERROR;
        ObString err_msg("raw variable length too long");
        LOG_WARN("raw variable length too long", K(ret), K(result.get_raw().length()));
        LOG_USER_ERROR(OB_ERR_NUMERIC_OR_VALUE_ERROR, err_msg.length(), err_msg.ptr());
      }
    }
    OZ (process_out_params(objs, expr.arg_cnt_,
                           *udf_params, alloc,
                           ctx.exec_ctx_, info->nocopy_params_,
                           info->params_desc_, info->params_type_));
    if (OB_SUCC(ret)) {
      //TODO:@peihan.dph
      //for lob locator type, pl engine return longtext type which is mismatch with datum type
      //we will solve this mismatch later
      if (!result.is_null()
          && result.get_type() != expr.datum_meta_.type_
          && ObLobType == expr.datum_meta_.type_) {
        ObLobLocator *value = nullptr;
        char *total_buf = NULL;
        const int64_t total_buf_len = sizeof(ObLobLocator) + result.get_string_len();
        if (OB_ISNULL(total_buf = expr.get_str_res_mem(ctx, total_buf_len))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("Failed to allocate memory for lob locator", K(ret), K(total_buf_len));
        } else if (FALSE_IT(value = reinterpret_cast<ObLobLocator *> (total_buf))) {
        } else if (OB_FAIL(value->init(result.get_string()))) {
          LOG_WARN("Failed to init lob locator", K(ret), K(value));
        } else {
          result.set_lob_locator(*value);
        }
      } else if (!result.is_null() && result.get_type() != expr.datum_meta_.type_) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("get unexpected result type", K(ret),
                         K(result.get_type()), K(expr.datum_meta_.type_));
      }
      OZ(res.from_obj(result, expr.obj_datum_map_));
      OZ(expr.deep_copy_datum(ctx, res));
    }
    if (need_end_stmt) {
      session->set_end_stmt();
    }
  }

  return ret;
}

int ObExprUDF::fill_obj_stack(const ObExpr &expr, ObEvalCtx &ctx, ObObj *objs)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < expr.arg_cnt_ && OB_SUCC(ret); ++i) {
    objs[i].reset();
    ObDatum &param = expr.args_[i]->locate_expr_datum(ctx);
    if (OB_FAIL(param.to_obj(objs[i], expr.args_[i]->obj_meta_))) {
      LOG_WARN("failed to convert obj", K(ret), K(i));
    }
  }
  return ret;


}

OB_DEF_SERIALIZE(ObExprUDFInfo)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE,
              udf_id_,
              udf_package_id_,
              subprogram_path_,
              result_type_,
              params_type_,
              params_desc_,
              nocopy_params_,
              is_udt_udf_,
              loc_,
              is_udt_cons_,
              is_called_in_sql_);
  return ret;
}

OB_DEF_DESERIALIZE(ObExprUDFInfo)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_DECODE,
              udf_id_,
              udf_package_id_,
              subprogram_path_,
              result_type_,
              params_type_,
              params_desc_,
              nocopy_params_,
              is_udt_udf_,
              loc_,
              is_udt_cons_,
              is_called_in_sql_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObExprUDFInfo)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              udf_id_,
              udf_package_id_,
              subprogram_path_,
              result_type_,
              params_type_,
              params_desc_,
              nocopy_params_,
              is_udt_udf_,
              loc_,
              is_udt_cons_,
              is_called_in_sql_);
  return len;
}

int ObExprUDFInfo::deep_copy(common::ObIAllocator &allocator,
                         const ObExprOperatorType type,
                         ObIExprExtraInfo *&copied_info) const
{
  int ret = common::OB_SUCCESS;
  OZ(ObExprExtraInfoFactory::alloc(allocator, type, copied_info));
  ObExprUDFInfo &other = *static_cast<ObExprUDFInfo *>(copied_info);
  other.udf_id_ = udf_id_;
  other.udf_package_id_ = udf_package_id_;
  other.result_type_ = result_type_;
  other.is_udt_udf_ = is_udt_udf_;
  other.loc_ = loc_;
  other.is_udt_cons_ = is_udt_cons_;
  other.is_called_in_sql_ = is_called_in_sql_;
  OZ(other.subprogram_path_.assign(subprogram_path_));
  OZ(other.params_type_.assign(params_type_));
  OZ(other.params_desc_.assign(params_desc_));
  OZ(other.nocopy_params_.assign(nocopy_params_));
  return ret;
}

template <typename RE>
int ObExprUDFInfo::from_raw_expr(RE &raw_expr)
{
  int ret = OB_SUCCESS;
  ObUDFRawExpr &udf_expr = const_cast<ObUDFRawExpr &> (static_cast<const ObUDFRawExpr&>(raw_expr));
  OZ(subprogram_path_.assign(udf_expr.get_subprogram_path()));
  OZ(params_type_.assign(udf_expr.get_params_type()));
  OZ(params_desc_.assign(udf_expr.get_params_desc()));
  OZ(nocopy_params_.assign(udf_expr.get_nocopy_params()));
  udf_id_ = udf_expr.get_udf_id();
  udf_package_id_ = udf_expr.get_pkg_id();
  result_type_ = udf_expr.get_result_type();
  is_udt_udf_ = udf_expr.get_is_udt_udf();
  loc_ = udf_expr.get_loc();
  is_udt_cons_ = udf_expr.get_is_udt_cons();
  return ret;
}

} /* sql */
} /* oceanbase */
