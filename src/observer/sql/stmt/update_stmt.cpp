/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/5/22.
// Completed by rrrrrzy on 2025/10/31
//

#include "sql/stmt/update_stmt.h"
#include "common/log/log.h"
#include "common/sys/rc.h"
#include "common/type/attr_type.h"
#include "common/value.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/field/field_meta.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"
#include <string>
#include <unordered_map>

UpdateStmt::UpdateStmt(Table *table, std::vector<Value> values, FieldMeta update_field, FilterStmt *filter_stmt)
    : table_(table), values_(std::move(values)), update_field_(update_field), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt()
{
  delete filter_stmt_;
  filter_stmt_ = nullptr;
}

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update, Stmt *&stmt)
{
  stmt = nullptr;
  const char *table_name = update.relation_name.c_str();
  if (nullptr == db || nullptr == table_name || update.value.attr_type() == AttrType::UNDEFINED) {
    LOG_WARN("invalid argument. db=%p, table_name=%p, value=%s",
        db, table_name, update.value.to_string().c_str());
    return RC::INVALID_ARGUMENT;
  }

  // 查看表名是否存在
  Table *table = db->find_table(table_name);
  if (!table) { // table == nullptr
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // 查看列名是否存在
  const char *field_name = update.attribute_name.c_str();
  const FieldMeta *field_meta = table->table_meta().field(field_name);
  if (nullptr == field_meta) {
    LOG_WARN("no such field in table. db=%s, table=%s, field name=%s", 
             db->name(), table_name, field_name);
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  FieldMeta update_field = *field_meta;

  // 检查类型正确
  // const TableMeta &table_meta = table->table_meta();
  // const int sys_field_num = table_meta.sys_field_num();
  const AttrType field_type = field_meta->type(), user_type = update.value.attr_type();
  if (field_type != user_type) {
    LOG_WARN("schema field type mismatch. db=%s, table=%s, field name=%s, field type=%s, your type=%s",
    db->name(), table_name, field_name,
    attr_type_to_string(field_type), attr_type_to_string(user_type));
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  // 生成FilterStmt（仅当存在 WHERE 条件）
  std::unordered_map<string, Table *> table_map;
  table_map.insert({std::string(table_name), table});

  FilterStmt *filter_stmt = nullptr;
  if (!update.conditions.empty()) {
    RC rc = FilterStmt::create(db,
        table,
        &table_map,
        update.conditions.data(),
        static_cast<int>(update.conditions.size()),
        filter_stmt);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot construct filter stmt. rc=%s", strrc(rc));
      return rc;
    }

    if (filter_stmt == nullptr || filter_stmt->filter_units().empty()) {
      LOG_WARN("no valid conditions generated filter units for update");
      delete filter_stmt;
      return RC::INVALID_ARGUMENT;
    }
  }

  std::vector<Value> values;
  values.emplace_back(update.value);

  stmt = new UpdateStmt(table, std::move(values), update_field, filter_stmt);
  return RC::SUCCESS;
}