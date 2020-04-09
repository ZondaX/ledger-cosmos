/*******************************************************************************
*   (c) 2018, 2019 Zondax GmbH
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include <jsmn.h>
#include <stdio.h>
#include "tx_parser.h"
#include "zxmacros.h"
#include "parser_impl.h"

// strcat but source does not need to be terminated (a chunk from a bigger string is concatenated)
// dst_max is measured in bytes including the space for NULL termination
// src_size does not include NULL termination
__always_inline void strcat_chunk_s(char *dst, uint16_t dst_max, const char *src_chunk, size_t src_chunk_size) {
    *(dst + dst_max - 1) = 0;                 // last character terminates with zero in case we go beyond bounds
    const size_t prev_size = strlen(dst);

    size_t space_left = dst_max - prev_size - 1;  // -1 because requires termination

    if (src_chunk_size > space_left) {
        src_chunk_size = space_left;
    }

    if (src_chunk_size > 0) {
        // Check bounds
        MEMCPY(dst + prev_size, src_chunk, src_chunk_size);
        // terminate
        *(dst + prev_size + src_chunk_size) = 0;
    }
}

///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////

static const key_subst_t value_substitutions[] = {
        {"cosmos-sdk/MsgSend",                     "Send"},
        {"cosmos-sdk/MsgDelegate",                 "Delegate"},
        {"cosmos-sdk/MsgUndelegate",               "Undelegate"},
        {"cosmos-sdk/MsgBeginRedelegate",          "Redelegate"},
        {"cosmos-sdk/MsgSubmitProposal",           "Propose"},
        {"cosmos-sdk/MsgDeposit",                  "Deposit"},
        {"cosmos-sdk/MsgVote",                     "Vote"},
        {"cosmos-sdk/MsgWithdrawDelegationReward", "Withdraw Reward"},
};

parser_error_t tx_getToken(uint16_t token_index,
                           char *out_val, uint16_t out_val_len,
                           uint8_t pageIdx, uint8_t *pageCount) {
    *pageCount = 0;
    MEMZERO(out_val, out_val_len);

    const int16_t token_start = parser_tx_obj.json.tokens[token_index].start;
    const int16_t token_end = parser_tx_obj.json.tokens[token_index].end;

    if (token_start > token_end) {
        return parser_unexpected_buffer_end;
    }

    const char *inValue = parser_tx_obj.tx + token_start;
    uint16_t inLen = token_end - token_start;

    // empty strings are considered the first page
    *pageCount = 1;
    if (inLen > 0) {
        for (uint8_t i = 0; i < array_length(value_substitutions); i++) {
            const char *substStr = value_substitutions[i].str1;
            const size_t substStrLen = strlen(substStr);
            if (inLen == substStrLen && !MEMCMP(inValue, substStr, substStrLen)) {
                inValue = value_substitutions[i].str2;
                inLen = strlen(value_substitutions[i].str2);
                break;
            }
        }

        pageStringExt(out_val, out_val_len,
                      inValue, inLen,
                      pageIdx, pageCount);

    }

    if (pageIdx >= *pageCount) {
        return parser_display_page_out_of_range;
    }

    return parser_ok;
}

__always_inline void append_key_item(int16_t token_index) {
    if (*parser_tx_obj.query.out_key > 0) {
        // There is already something there, add separator
        strcat_chunk_s(parser_tx_obj.query.out_key,
                       parser_tx_obj.query.out_key_len,
                       "/",
                       1);
    }

    const int16_t token_start = parser_tx_obj.json.tokens[token_index].start;
    const int16_t token_end = parser_tx_obj.json.tokens[token_index].end;
    const char *address_ptr = parser_tx_obj.tx + token_start;
    const int16_t new_item_size = token_end - token_start;

    strcat_chunk_s(parser_tx_obj.query.out_key,
                   parser_tx_obj.query.out_key_len,
                   address_ptr,
                   new_item_size);
}
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////

parser_error_t tx_traverse_find(int16_t root_token_index, uint16_t *ret_value_token_index) {
    parser_error_t err;
    const jsmntype_t token_type = parser_tx_obj.json.tokens[root_token_index].type;

    if (parser_tx_obj.tx == NULL || root_token_index < 0) {
        return parser_no_data;
    }

    if (parser_tx_obj.query.max_level <= 0 || parser_tx_obj.query.max_depth <= 0 ||
        token_type == JSMN_STRING ||
        token_type == JSMN_PRIMITIVE) {

        const bool groupedField = strcmp("msgs/type", parser_tx_obj.query.out_key) == 0;
        const bool isMainIndex = parser_tx_obj.filter_msg_type_valid_idx != parser_tx_obj.query._item_index_current;
        const bool skipItem = parser_tx_obj.flags.cache_valid == 1u &&
                              parser_tx_obj.flags.msg_type_grouping == 1u &&
                              groupedField &&
                              isMainIndex;

        // Early bail out
        if (!skipItem && parser_tx_obj.query._item_index_current == parser_tx_obj.query.item_index) {
            *ret_value_token_index = root_token_index;
            return parser_ok;
        }

        if (skipItem) {
            parser_tx_obj.query.item_index++;
        }

        parser_tx_obj.query._item_index_current++;
        return parser_query_no_results;
    }

    const int16_t el_count = object_get_element_count(root_token_index, &parser_tx_obj.json);

    switch (token_type) {
        case JSMN_OBJECT: {
            const size_t key_len = strlen(parser_tx_obj.query.out_key);
            for (int16_t i = 0; i < el_count; ++i) {
                const int16_t key_index = object_get_nth_key(root_token_index, i, &parser_tx_obj.json);
                const int16_t value_index = object_get_nth_value(root_token_index, i, &parser_tx_obj.json);

                // Skip writing keys if we are actually exploring to count
                append_key_item(key_index);

                // When traversing objects both level and depth should be considered
                parser_tx_obj.query.max_level--;
                parser_tx_obj.query.max_depth--;
                // Traverse the value, extracting subkeys
                err = tx_traverse_find(value_index, ret_value_token_index);
                parser_tx_obj.query.max_level++;
                parser_tx_obj.query.max_depth++;

                if (err == parser_ok)
                    return err;

                *(parser_tx_obj.query.out_key + key_len) = 0;
            }
            break;
        }
        case JSMN_ARRAY: {
            for (int16_t i = 0; i < el_count; ++i) {
                const int16_t element_index = array_get_nth_element(root_token_index, i,
                                                                    &parser_tx_obj.json);

                // When iterating along an array,
                // the level does not change but we need to count the recursion
                parser_tx_obj.query.max_depth--;
                err = tx_traverse_find(element_index, ret_value_token_index);
                parser_tx_obj.query.max_depth++;

                if (err == parser_ok)
                    return err;
            }
            break;
        }
        default:
            break;
    }

    return parser_query_no_results;
}