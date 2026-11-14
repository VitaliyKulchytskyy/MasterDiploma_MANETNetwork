#ifndef MERLD_CHAT_H
#define MERLD_CHAT_H

// Chat history in SQLite: user messages sent and received over ERL (MERL_CMD_USER_MESSAGE_SEND)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"

bool chat_open(const char* path);
void chat_close(void);

/**
 * Store a message.
 *
 * @param dir    "in" (received over ERL) or "out" (sent by this unit).
 * @param status "received" for in; "pending", then "sent" or "failed: ..." for out.
 * @return Row id, or -1 on error.
 */
int64_t chat_add(const char* dir, uint16_t sender, uint16_t receiver, bool broadcast, const uint8_t* text,
                 size_t length, const char* status);

void chat_set_status(int64_t id, const char* status);

int64_t chat_last_id(void);
long chat_count(void);

/**
 * Delete every message. Ids go on from the highest one given so far.
 *
 * @return How many were deleted, or -1 on error.
 */
long chat_clear(void);

/**
 * Check the open database: SQLite integrity and the columns merld uses (e.g. a restored file).
 * Reports problems on stderr.
 */
bool chat_check(void);

/**
 * Append a JSON array of messages, oldest first.
 *
 * @param after Only messages with a larger id; 0 or less: the newest @p limit messages.
 * @param limit At most this many.
 */
void chat_json(struct buf* b, int64_t after, int limit);

#endif // MERLD_CHAT_H
