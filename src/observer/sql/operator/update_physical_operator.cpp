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
// Created by WangYunlai on 2021/6/9.
//

#include "sql/operator/update_physical_operator.h"

#include "common/log/log.h"
#include "common/type/attr_type.h"
#include "sql/expr/tuple.h"
#include "storage/record/record.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

#include <cstring>
#include <vector>

using namespace std;

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const FieldMeta &field, std::vector<Value> &&values)
    : table_(table), target_field_(field)
{
  if (values.size() != 1) {
    LOG_WARN("update operator currently supports single-column updates only, actual column count=%zu", values.size());
  }

  if (!values.empty()) {
    new_value_ = values.front();
  } else {
    LOG_WARN("no value provided for update operator");
  }
}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    LOG_WARN("update operator requires one child but got none");
    return RC::INTERNAL;
  }

  unique_ptr<PhysicalOperator> &child = children_.front();

  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator for update. rc=%s", strrc(rc));
    return rc;
  }

  affected_rows_ = 0;

  struct PendingUpdate
  {
    Record old_record;
    Record new_record;
  };

  std::vector<PendingUpdate> pending_updates;

  while (true) {
    rc = child->next();
    if (rc == RC::RECORD_EOF) {
      rc = RC::SUCCESS;
      break;
    }

    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to iterate child operator for update. rc=%s", strrc(rc));
      break;
    }

    Tuple *tuple = child->current_tuple();
    if (nullptr == tuple) {
      LOG_WARN("child operator returned null tuple");
      rc = RC::INTERNAL;
      break;
    }

    auto *row_tuple = dynamic_cast<RowTuple *>(tuple);
    if (nullptr == row_tuple) {
      LOG_WARN("unexpected tuple type for update operator");
      rc = RC::INTERNAL;
      break;
    }

    Record &stream_record = row_tuple->record();

    PendingUpdate update;
    rc = update.old_record.copy_data(stream_record.data(), stream_record.len());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to copy original record data. rc=%s", strrc(rc));
      break;
    }
    update.old_record.set_rid(stream_record.rid());
    update.old_record.set_key(stream_record.key());

    rc = update.new_record.copy_data(stream_record.data(), stream_record.len());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to copy record data for update. rc=%s", strrc(rc));
      break;
    }
    update.new_record.set_rid(stream_record.rid());
    update.new_record.set_key(stream_record.key());

    rc = apply_new_value(update.new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to apply new value when preparing update. rc=%s", strrc(rc));
      break;
    }

    pending_updates.emplace_back(std::move(update));
  }

  RC close_rc = child->close();
  if (close_rc != RC::SUCCESS) {
    LOG_WARN("failed to close child operator for update. rc=%s", strrc(close_rc));
    if (rc == RC::SUCCESS) {
      rc = close_rc;
    }
  }

  if (rc != RC::SUCCESS) {
    return rc;
  }

  for (PendingUpdate &update : pending_updates) {
    rc = trx->update_record(table_, update.old_record, update.new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record by transaction. rc=%s", strrc(rc));
      break;
    }
    affected_rows_++;
  }

  return rc;
}

RC UpdatePhysicalOperator::next() { return RC::RECORD_EOF; }

RC UpdatePhysicalOperator::close()
{
  affected_rows_ = 0;
  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::apply_new_value(Record &record) const
{
  if (record.data() == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  AttrType field_type = target_field_.type();

  // TODO: support variable-length or LOB fields that require dedicated codecs.

  Value cast_value;
  const Value *value_ptr = &new_value_;
  if (new_value_.attr_type() != field_type) {
    RC rc = Value::cast_to(new_value_, field_type, cast_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to cast value for update. field=%s, expect=%s, actual=%s",
          target_field_.name(), attr_type_to_string(field_type), attr_type_to_string(new_value_.attr_type()));
      return rc;
    }
    value_ptr = &cast_value;
  }

  char *field_ptr = record.data() + target_field_.offset();
  const int field_len = target_field_.len();

  std::memset(field_ptr, 0, field_len);

  if (field_type == AttrType::CHARS) {
    size_t copy_len = field_len;
    size_t data_len = value_ptr->length();
    if (copy_len > data_len) {
      copy_len = data_len + 1;
    }
    std::memcpy(field_ptr, value_ptr->data(), copy_len);
  } else {
    std::memcpy(field_ptr, value_ptr->data(), field_len);
  }

  return RC::SUCCESS;
}
