#include "message_queue.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

/**
 * TODO look into using eventfd instead of a pipe if we have it available XXX
 */

/**
 * @param mq A pointer to allocated space for a libtrace message queue
 * @param message_len The size in bytes of the message item, to ensure thread
 * safety this should be less than PIPE_BUF (normally at least 512bytes) see:
 * man 7 pipe notes on atomic operations
 */
void libtrace_message_queue_init(libtrace_message_queue_t *mq,
                                 size_t message_len)
{
  assert(message_len);
  int rc = pipe(mq->pipefd);
  assert(rc == 0); // AK: silence compiler warning about unused return val
  mq->message_count = 0;
  if (message_len > PIPE_BUF)
    fprintf(stderr,
            "Warning message queue wont be atomic (thread safe) "
            "message_len(%zu) > PIPE_BUF(%d)\n",
            message_len, PIPE_BUF);
  mq->message_len = message_len;
  pthread_spin_init(&mq->spin, 0);
}

/**
 * Posts a message to the given message queue.
 *
 * This will block if a reader is not keeping up and the underlying pipe
 * fills up.
 *
 * @param mq A pointer to a initilised libtrace message queue structure (NOT
 * NULL)
 * @param message A pointer to the message data you wish to send
 * @return A number representing the number of messages already in the queue,
 *         0 implies a thread was waiting and will read your message, negative
 *         numbers implies threads are still waiting. Positive implies a backlog
 *         of messages.
 */
int libtrace_message_queue_put(libtrace_message_queue_t *mq,
                               const void *message)
{
  int ret;
  assert(mq->message_len);
  ret = write(mq->pipefd[1], message, mq->message_len);
  assert(ret == mq->message_len); // AK: silence compiler warning about
                                  // unused return val
  // Update after we've written
  pthread_spin_lock(&mq->spin);
  ret = ++mq->message_count; // Should be CAS!
  pthread_spin_unlock(&mq->spin);
  return ret;
}

/**
 * Retrieves a message from the given message queue.
 *
 * This will block if a reader is not keeping up and the underlying pipe
 * fills up.
 *
 * @param mq A pointer to a initilised libtrace message queue structure (NOT
 * NULL)
 * @param message A pointer to the message data you wish to send
 * @return The number of messages remaining in the queue less any threads
 * waiting, 0 implies a thread was waiting and will read your message, negative
 *         numbers implies threads are still waiting. Positive implies a backlog
 *         of messages.
 */
int libtrace_message_queue_get(libtrace_message_queue_t *mq, void *message)
{
  int ret;
  // Safely decrease count first - Yes this might make us negative, however
  // thats ok once a write comes in everything will be fine
  pthread_spin_lock(&mq->spin);
  ret = mq->message_count--;
  pthread_spin_unlock(&mq->spin);
  ssize_t rc = read(mq->pipefd[0], message, mq->message_len);
  assert(rc >= 0); // AK: better check needed here
  return ret;
}

/**
 * Trys to retrieve a message from the given message queue.
 *
 * This will not block and instead returns LIBTRACE_MQ_FAILED if
 * no message is available.
 *
 * @param mq A pointer to a initilised libtrace message queue structure (NOT
 * NULL)
 * @param message A pointer to the message data you wish to send
 * @return The number of messages remaining in the queue less any threads
 * waiting, 0 implies a thread was waiting and will read your message, negative
 *         numbers implies threads are still waiting. Positive implies a backlog
 *         of messages.
 */
int libtrace_message_queue_try_get(libtrace_message_queue_t *mq, void *message)
{
  int ret;
  // Safely decrease count first - Yes this might make us negative, however
  // thats ok once a write comes in everything will be fine
  // ->Fast path avoid the lock
  if (mq->message_count <= 0)
    return LIBTRACE_MQ_FAILED;
  // Else grab lock and confirm this is so
  pthread_spin_lock(&mq->spin);
  if (mq->message_count > 0) {
    ret = --mq->message_count;
    // :( read(...) needs to be done within the *spin* lock otherwise blocking
    // might steal our read
    ssize_t rc = read(mq->pipefd[0], message, mq->message_len);
    assert(rc >= 0);
  } else {
    ret = LIBTRACE_MQ_FAILED;
  }
  pthread_spin_unlock(&mq->spin);
  return ret;
}

/**
 * May be negative if threads blocking and waiting for a message.
 */
int libtrace_message_queue_count(const libtrace_message_queue_t *mq)
{
  // This is only ok because we know int is atomic
  return mq->message_count;
}

void libtrace_message_queue_destroy(libtrace_message_queue_t *mq)
{
  mq->message_count = 0;
  mq->message_len = 0;
  close(mq->pipefd[0]);
  close(mq->pipefd[1]);
  pthread_spin_destroy(&mq->spin);
}

/**
 * @return a file descriptor for the queue, can be used with select() poll()
 * etc.
 */
int libtrace_message_queue_get_fd(libtrace_message_queue_t *mq)
{
  return mq->pipefd[0];
}
