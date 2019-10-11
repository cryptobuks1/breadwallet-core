//
//  BREvent.c
//  BRCore
//
//  Created by Ed Gamble on 5/7/18.
//  Copyright © 2018-2019 Breadwinner AG.  All rights reserved.
//
//  See the LICENSE file at the project root for license information.
//  See the CONTRIBUTORS file at the project root for a list of contributors.

#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <assert.h>
#include "BREvent.h"
#include "BREventQueue.h"
#include "BREventAlarm.h"

#define PTHREAD_NULL   ((pthread_t) NULL)
#define PTHREAD_STACK_SIZE (512 * 1024)
#define PTHREAD_NAME_SIZE   (33)

/* Forward Declarations */
static void *
eventHandlerThread (BREventHandler handler);

//
// Event Handler
//
struct BREventHandlerRecord {
    char name[PTHREAD_NAME_SIZE];

    // Types
    size_t typesCount;
    const BREventType **types;

    // Queue
    size_t eventSize;
    BREventQueue queue;
    BREvent *scratch;

    // (Optional) Timeout

    ///
    /// The Handler specific timeout event - `filled` with the dispatcher
    ////
    BREventType timeoutEventType;

    ///
    /// The Handler specific timeout context.
    ///
    BREventTimeoutContext timeoutContext;

    ///
    /// The timeout period
    ///
    struct timespec timeout;

    ///
    /// The timeout alarm id, if one exists
    ///
    BREventAlarmId timeoutAlarmId;

    // The thread handling events.
    pthread_t thread;

    // A cond-var for exiting the thread.
    pthread_cond_t threadExit;

    // A lock on internal state
    pthread_mutex_t lock;

    // A lock for protecting the dispatch call.  Optional but recommended.
    pthread_mutex_t *lockOnDispatch;
};

extern BREventHandler
eventHandlerCreate (const char *name,
                    const BREventType *types[],
                    unsigned int typesCount,
                    pthread_mutex_t *lockOnDispatch) {
    BREventHandler handler = calloc (1, sizeof (struct BREventHandlerRecord));

    // Fill in the timeout event.  Leave the dispatcher NULL until the dispatcher is provided.
    handler->timeoutEventType.eventName = "Timeout Event";
    handler->timeoutEventType.eventSize = sizeof(BREventTimeout);
    handler->timeoutEventType.eventDispatcher = NULL;

    // Handle the event types.  Ensure we account for the (implicit) timeout event.
    handler->typesCount = typesCount;
    handler->types = types;
    handler->eventSize = handler->timeoutEventType.eventSize;

    strlcpy (handler->name, name, PTHREAD_NAME_SIZE);

    // Update `eventSize` with the largest sized event
    for (int i = 0; i < handler->typesCount; i++) {
        const BREventType *type = handler->types[i];

        if (handler->eventSize < type->eventSize)
            handler->eventSize = type->eventSize;
    }

    handler->timeoutAlarmId = ALARM_ID_NONE;
    handler->lockOnDispatch = lockOnDispatch;

    // Create the PTHREAD LOCK variable
    {
        // The cacheLock is a normal, non-recursive lock
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
        pthread_mutex_init(&handler->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_cond_init(&handler->threadExit, &attr);
        pthread_condattr_destroy(&attr);
    }

    handler->thread = PTHREAD_NULL;

    handler->scratch = (BREvent*) calloc (1, handler->eventSize);
    handler->queue = eventQueueCreate (handler->eventSize);

    return handler;
}

extern void
eventHandlerSetTimeoutDispatcher (BREventHandler handler,
                                  unsigned int timeInMilliseconds,
                                  BREventDispatcher dispatcher,
                                  BREventTimeoutContext context) {
    pthread_mutex_lock (&handler->lock);
    handler->timeout.tv_sec = timeInMilliseconds / 1000;
    handler->timeout.tv_nsec = 1000000 * (timeInMilliseconds % 1000);
    handler->timeoutContext = context;
    handler->timeoutEventType.eventDispatcher = dispatcher;
    pthread_mutex_unlock (&handler->lock);
}

static void
eventHandlerAlarmCallback (BREventHandler handler,
                           struct timespec expiration,
                           BREventAlarmClock clock) {
    BREventTimeout event =
    { { NULL, &handler->timeoutEventType }, handler->timeoutContext, expiration};
    eventHandlerSignalEventOOB (handler, (BREvent*) &event);
}

typedef void* (*ThreadRoutine) (void*);

static void *
eventHandlerThread (BREventHandler handler) {
#if defined (__ANDROID__)
    pthread_setname_np (pthread_self(), handler->name);
#else
    pthread_setname_np (handler->name);
#endif
    int timeToQuit = 0;

    // We have `pthread_self()` here, surely.  We'll assign it to `handler->thread` thereby
    // ensuring that every event dispatcher has it, if needed. But that assignment must be done
    // with `handler->lock` held.
    pthread_mutex_lock(&handler->lock);
    if (PTHREAD_NULL == handler->thread) handler->thread = pthread_self();
    pthread_mutex_unlock(&handler->lock);

    while (!timeToQuit) {
        // Check for a queued event
        switch (eventQueueDequeueWait (handler->queue, handler->scratch)) {
            case EVENT_STATUS_SUCCESS:
                // We got an event, dispatch
                if (handler->lockOnDispatch) pthread_mutex_lock (handler->lockOnDispatch);
                handler->scratch->type->eventDispatcher (handler, handler->scratch);
                if (handler->lockOnDispatch) pthread_mutex_unlock (handler->lockOnDispatch);
                break;

            case EVENT_STATUS_WAIT_ABORT:
                timeToQuit = 1;
                break;

            case EVENT_STATUS_WAIT_ERROR:
                // Just try again.
                break;

            case EVENT_STATUS_NOT_STARTED:
            case EVENT_STATUS_UNKNOWN_TYPE:
            case EVENT_STATUS_NULL_EVENT:
            case EVENT_STATUS_NONE_PENDING:
                assert (0);
                break;
        }
    }

    pthread_mutex_lock(&handler->lock);
    if (PTHREAD_NULL != handler->thread) handler->thread = PTHREAD_NULL;
    pthread_cond_signal (&handler->threadExit);
    pthread_mutex_unlock(&handler->lock);

    return NULL;
}

extern void
eventHandlerDestroy (BREventHandler handler) {
    // First stop...
    eventHandlerStop(handler);

    // ... then kill
    pthread_mutex_lock (&handler->lock);
    assert (PTHREAD_NULL == handler->thread);
    pthread_mutex_unlock  (&handler->lock);
    pthread_mutex_destroy (&handler->lock);

    // release memory
    eventQueueDestroy(handler->queue);
    free (handler->scratch);
    free (handler);
}

//
// Start / Stop
//

/**
 * Start the handler.  It is possible that events will already be queued; they will all be
 * dispatched, in FIFO order.  If there is a periodic alarm; it will be added to the alarmClock.
 *
 * @param handler
 */
extern void
eventHandlerStart (BREventHandler handler) {
    alarmClockCreateIfNecessary(1);
    pthread_mutex_lock(&handler->lock);
    if (PTHREAD_NULL == handler->thread) {
        // If we have an timeout event dispatcher, then add an alarm.
        if (NULL != handler->timeoutEventType.eventDispatcher) {
            handler->timeoutAlarmId = alarmClockAddAlarmPeriodic (alarmClock,
                                                                  (BREventAlarmContext) handler,
                                                                  (BREventAlarmCallback) eventHandlerAlarmCallback,
                                                                  handler->timeout);
        }

        // Spawn the eventHandlerThread
        {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
            pthread_attr_setstacksize(&attr, PTHREAD_STACK_SIZE);

            pthread_create (&handler->thread, &attr, (ThreadRoutine) eventHandlerThread, handler);
            // "Before returning, a successful call to pthread_create() stores the ID of the new
            // thread in the buffer pointed to by thread; this identifier is used to refer to the
            // thread in subsequent calls to other pthreads functions."
            //
            // It is possible (apparently, by observation), that we arrive here whereby
            // `eventHandlerThread` has not only run, but has also dequeued an event and
            // dispatched on it.  That dispatched function may have attempted to access
            // `handler->thread`.  Thus, somehow we need to ensure that `eventHandlerThread`
            // doesn't get so far along that `handler->thread` is referenced before it is
            // assigned.  Thankfully, we hold `handler->lock` here and can use it to prevent
            // `eventHandlerThread` from running until `pthread_create()` returns...
            //
            // TODO: Handle an unsuccessfull call.
            // assert (NULL != handler->thread);

            pthread_attr_destroy(&attr);
        }
    }
    pthread_mutex_unlock(&handler->lock);
}


/**
 * Stop the handler.  This will clear all pending events.  If there is a periodic alarm, it will
 * be removed from the alarmClock.
 *
 * @note There is a tiny race here, I think.  Before this function returns and after the queue
 * has been cleared, another event can be added.  This is prevented by stopping the threads that
 * submit to this queue before stopping this queue's thread.  Or prior to a subsequent start,
 * clear this handler (But, `eventHandlerStart()` does not clear the thread on start.)
 *
 * @param handler
 */
extern void
eventHandlerStop (BREventHandler handler) {
    pthread_mutex_lock(&handler->lock);
    if (PTHREAD_NULL != handler->thread) {
        // Remove a timeout alarm, if it exists.
        if (ALARM_ID_NONE != handler->timeoutAlarmId) {
            alarmClockRemAlarm (alarmClock, handler->timeoutAlarmId);
            handler->timeoutAlarmId = ALARM_ID_NONE;
        }

        // Quit the thread by aborting the queue wait.
        eventQueueDequeueWaitAbort (handler->queue);

        // Wait for the thread.  We must release the lock here because the `eventHandlerThread`
        // could itself be waiting on the lock (only as part of a running, dispatched function).
        // Once we give the lock, the dispatched function will complete, the `eventHandlerThread`
        // will return to the EventQueue and see the above 'Wait Abort' cond-var signaled.  At
        // that point, `eventHandlerThread` will finish and signal 'threadExit`.  The following
        // cond_wait will complete and reaquire the lock.
        //
        // Note, while we wait we will not hold the lock but handle->thread will be non-NULL; so,
        // we don't have a start/stop race.
        //
        pthread_cond_wait (&handler->threadExit, &handler->lock);

        // TODO: Empty the queue completely?  Or not?
        eventQueueDequeueWaitAbortReset (handler->queue);
        eventHandlerClear (handler);
    }
    pthread_mutex_unlock(&handler->lock);
}

extern int
eventHandlerIsCurrentThread (BREventHandler handler) {
    pthread_mutex_lock(&handler->lock);
    int result = (pthread_self() == handler->thread);
    pthread_mutex_unlock(&handler->lock);
    return result;
}

extern int
eventHandlerIsRunning (BREventHandler handler) {
    pthread_mutex_lock(&handler->lock);
    int result = (PTHREAD_NULL != handler->thread);
    pthread_mutex_unlock(&handler->lock);
    return result;
}

extern BREventStatus
eventHandlerSignalEvent (BREventHandler handler,
                         BREvent *event) {
    eventQueueEnqueueTailSignal (handler->queue, event);
    return EVENT_STATUS_SUCCESS;
}

extern BREventStatus
eventHandlerSignalEventOOB (BREventHandler handler,
                            BREvent *event) {
    eventQueueEnqueueHeadSignal (handler->queue, event);
    return EVENT_STATUS_SUCCESS;
}

extern void
eventHandlerClear (BREventHandler handler) {
    eventQueueClear(handler->queue);
}
