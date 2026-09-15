/* Exercise the production conversation repairs without an LLM or network.
 * Link the normal library objects (without agent.o/main.o), or dead-strip
 * llm.c/json_util.c/json_fast.c for a small sanitizer-enabled standalone run. */
#include "llm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static msg_content_t *add_block(message_t *message, const char *type, const char *id) {
    message->content = realloc(message->content,
                              (size_t)(message->content_count + 1) * sizeof(msg_content_t));
    assert(message->content);
    msg_content_t *block = &message->content[message->content_count++];
    memset(block, 0, sizeof(*block));
    block->type = strdup(type);
    block->tool_id = strdup(id);
    block->tool_name = strdup("bash");
    if (strcmp(type, "tool_use") == 0) block->tool_input = strdup("{}");
    else block->text = strdup("preserved real output");
    return block;
}

static int count_results(const conversation_t *conversation, const char *id) {
    int count = 0;
    for (int i = 0; i < conversation->count; ++i)
        for (int j = 0; j < conversation->msgs[i].content_count; ++j) {
            const msg_content_t *block = &conversation->msgs[i].content[j];
            count += block->type && strcmp(block->type, "tool_result") == 0 &&
                     block->tool_id && strcmp(block->tool_id, id) == 0;
        }
    return count;
}

static void require_integrity(conversation_t *conversation) {
    tool_integrity_result_t integrity = conv_validate_tool_call_integrity(conversation, false);
    if (!integrity.ok) fprintf(stderr, "integrity: %s\n", integrity.first_error);
    assert(integrity.ok);
}

static void repair_historical_interruption(void) {
    conversation_t conversation;
    conv_init(&conversation);
    conv_add_user_text(&conversation, "original task");
    conv_add_assistant_tool_use(&conversation, "interrupted", "bash", "{}");
    conv_add_user_text(&conversation, "continue after interruption");
    conv_add_assistant_text(&conversation, "continuing");
    conv_add_user_text(&conversation, "latest user instruction");
    conv_ensure_tool_results(&conversation);
    require_integrity(&conversation);
    assert(count_results(&conversation, "interrupted") == 1);
    assert(strcmp(conversation.msgs[2].content[0].tool_id, "interrupted") == 0);
    assert(conversation.msgs[2].content[0].is_error);
    assert(strcmp(conversation.msgs[conversation.count - 1].content[0].text,
                  "latest user instruction") == 0);
    int count = conversation.count;
    conv_ensure_tool_results(&conversation);
    assert(conversation.count == count);
    assert(count_results(&conversation, "interrupted") == 1);
    conv_free(&conversation);
}

static void repair_reallocation_and_parallel_calls(void) {
    conversation_t conversation;
    conv_init(&conversation);
    conv_add_user_text(&conversation, "run parallel commands");
    conv_add_assistant_tool_use(&conversation, "missing_a", "bash", "{}");
    add_block(&conversation.msgs[1], "tool_use", "present");
    add_block(&conversation.msgs[1], "tool_use", "missing_b");
    conv_add_tool_result_named(&conversation, "present", "bash", "preserved real output", false);
    conv_add_assistant_text(&conversation, "resume");
    /* Force the repair to grow the message array and exercise pointer lifetime. */
    conversation.msgs = realloc(conversation.msgs,
                                (size_t)conversation.count * sizeof(message_t));
    assert(conversation.msgs);
    conversation.cap = conversation.count;
    conv_ensure_tool_results(&conversation);
    require_integrity(&conversation);
    assert(count_results(&conversation, "missing_a") == 1);
    assert(count_results(&conversation, "missing_b") == 1);
    assert(count_results(&conversation, "present") == 1);
    bool preserved = false;
    for (int i = 0; i < conversation.count; ++i)
        for (int j = 0; j < conversation.msgs[i].content_count; ++j) {
            const msg_content_t *block = &conversation.msgs[i].content[j];
            preserved |= block->text && strcmp(block->text, "preserved real output") == 0 &&
                         !block->is_error;
        }
    assert(preserved);
    int count = conversation.count;
    conv_ensure_tool_results(&conversation);
    assert(conversation.count == count);
    conv_free(&conversation);
}

static void repair_multiple_interruptions_and_pending_tail(void) {
    conversation_t conversation;
    conv_init(&conversation);
    conv_ensure_tool_results(NULL);
    conv_ensure_tool_results(&conversation);
    assert(conversation.count == 0);
    for (int i = 0; i < 40; ++i) {
        char id[32];
        snprintf(id, sizeof(id), "interrupted_%d", i);
        conv_add_user_text(&conversation, "continue");
        conv_add_assistant_tool_use(&conversation, id, "bash", "{}");
    }
    conv_ensure_tool_results(&conversation);
    require_integrity(&conversation);
    assert(conversation.count == 120);
    for (int i = 0; i < 40; ++i) {
        char id[32];
        snprintf(id, sizeof(id), "interrupted_%d", i);
        assert(count_results(&conversation, id) == 1);
        assert(strcmp(conversation.msgs[3 * i + 2].content[0].tool_id, id) == 0);
    }
    conv_ensure_tool_results(&conversation);
    assert(conversation.count == 120);
    conv_free(&conversation);
}

static void compaction_requires_complete_exchange(void) {
    conversation_t conversation;
    conv_init(&conversation);
    conv_add_user_text(&conversation, "run both tools");
    conv_add_assistant_tool_use(&conversation, "first", "bash", "{}");
    add_block(&conversation.msgs[1], "tool_use", "second");
    conv_add_tool_result(&conversation, "first", "one", false);
    assert(!conv_compact_recent_tool_turn(&conversation, 256, 0));
    assert(strcmp(conversation.msgs[1].content[0].type, "tool_use") == 0);
    /* Multiple user messages may carry the outputs of a parallel call batch. */
    conv_add_user_text(&conversation, "interleaved user instruction");
    add_block(&conversation.msgs[3], "tool_result", "second");
    require_integrity(&conversation);
    assert(!conv_compact_recent_tool_turn(&conversation, 256, 0));
    require_integrity(&conversation);
    conv_free(&conversation);

    conv_init(&conversation);
    conv_add_assistant_tool_use(&conversation, "expected", "bash", "{}");
    conv_add_tool_result(&conversation, "wrong_id", "unrelated", false);
    assert(!conv_compact_recent_tool_turn(&conversation, 256, 0));
    assert(strcmp(conversation.msgs[0].content[0].type, "tool_use") == 0);
    conv_free(&conversation);

    conv_init(&conversation);
    conv_add_assistant_tool_use(&conversation, "duplicate", "bash", "{}");
    conv_add_tool_result(&conversation, "duplicate", "one", false);
    conv_add_tool_result(&conversation, "duplicate", "two", false);
    assert(!conv_compact_recent_tool_turn(&conversation, 256, 0));
    assert(conversation.msgs[1].content_count == 2);
    conv_free(&conversation);

    conv_init(&conversation);
    conv_add_assistant_tool_use(&conversation, "duplicate", "bash", "{}");
    add_block(&conversation.msgs[0], "tool_use", "duplicate");
    conv_add_tool_result(&conversation, "duplicate", "one", false);
    assert(!conv_compact_recent_tool_turn(&conversation, 256, 0));
    assert(conversation.msgs[0].content_count == 2);
    conv_free(&conversation);

    conv_init(&conversation);
    conv_add_assistant_tool_use(&conversation, "complete", "bash", "{}");
    add_block(&conversation.msgs[0], "tool_use", "complete_parallel");
    conv_add_tool_result(&conversation, "complete", "valid result", false);
    conv_add_tool_result(&conversation, "complete_parallel", "valid second result", false);
    conv_ensure_tool_results(&conversation);
    assert(conversation.count == 2); /* Valid exchanges are untouched. */
    assert(conv_compact_recent_tool_turn(&conversation, 256, 0));
    require_integrity(&conversation);
    assert(strstr(conversation.msgs[1].content[0].text, "valid result"));
    assert(strstr(conversation.msgs[1].content[0].text, "valid second result"));
    conv_free(&conversation);
}

int main(void) {
    repair_historical_interruption();
    repair_reallocation_and_parallel_calls();
    repair_multiple_interruptions_and_pending_tail();
    compaction_requires_complete_exchange();
    puts("context normalization: interrupted history, parallel outputs, allocation growth, "
         "idempotency, and compaction graph integrity passed");
    return 0;
}
