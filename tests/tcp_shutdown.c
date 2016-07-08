/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "../src/nn.h"
#include "../src/pair.h"
#include "../src/pubsub.h"
#include "../src/pipeline.h"
#include "../src/tcp.h"

#include "testutil.h"
#include "../src/utils/attr.h"
#include "../src/utils/thread.c"
#include "../src/utils/atomic.c"

/*  Stress test the TCP transport. */

#define THREAD_COUNT 10
#define TEST2_THREAD_COUNT 10
#define MESSAGES_PER_THREAD 10
#define TEST_LOOPS 10
#define MSG "hello"
#define MSG_LEN sizeof (MSG) - 1

struct nn_atomic active;

static char socket_address [128];

static void routine (NN_UNUSED void *arg)
{
    int s;

    nn_clear_errno ();
    s = nn_socket (AF_SP, NN_SUB);
    if (s < 0) {
        nn_assert_is_error (s == -1, EMFILE);
        return;
    }
    test_connect (s, socket_address);
    test_close (s);
}

static void routine2 (NN_UNUSED void *arg)
{
    int ms;
    int s;
    int i;

    s = test_socket (AF_SP, NN_PULL);

    ms = 1000;
    test_setsockopt (s, NN_SOL_SOCKET, NN_RCVTIMEO, &ms, sizeof (ms));
    
    test_connect (s, socket_address);

    for (i = 0; i != MESSAGES_PER_THREAD; ++i) {
        nn_yield ();
        test_recv (s, MSG);
    }

    test_close (s);
    nn_atomic_dec (&active, 1);
}

int main (int argc, const char *argv[])
{
    struct nn_thread threads [THREAD_COUNT];
    int count;
    int rc;
    int sb;
    int ms;
    int i;
    int j;

    test_addr_from (socket_address, "tcp", "127.0.0.1",
            get_test_port (argc, argv));

    /*  Stress the shutdown algorithm. */

#if defined(SIGPIPE) && defined(SIG_IGN)
    signal (SIGPIPE, SIG_IGN);
#endif

    sb = test_socket (AF_SP, NN_PUB);
    test_bind (sb, socket_address);

    for (j = 0; j != TEST_LOOPS; ++j) {
        for (i = 0; i != THREAD_COUNT; ++i) {
            nn_thread_init (&threads [i], routine, NULL);
        }
        for (i = 0; i != THREAD_COUNT; ++i) {
            nn_thread_term (&threads [i]);
        }
    }

    test_close (sb);

    /*  Test race condition of sending message while socket shutting down  */

    sb = test_socket (AF_SP, NN_PUSH);

    ms = 10;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_SNDTIMEO, &ms, sizeof (ms));
    nn_assert (ms == 10);

    test_bind (sb, socket_address);

    for (j = 0; j != TEST_LOOPS; ++j) {
        nn_atomic_init (&active, TEST2_THREAD_COUNT);
        for (i = 0; i != TEST2_THREAD_COUNT; ++i) {
            nn_thread_init (&threads [i], routine2, NULL);
        }
        
        nn_sleep (100);

        /*  Loop until the first timeout indicating all workers are gone. */
        count = 0;
        while (1) {
            nn_clear_errno ();
            rc = nn_send (sb, MSG, MSG_LEN, 0);
            if (rc == MSG_LEN) {
                nn_yield ();
                count++;
                continue;
            }
            nn_assert_is_error (rc == -1 && !active.n, ETIMEDOUT);
            break;
        }

        /*  Once all workers are gone, ensure that the total number of messages
            sent is at least the total expected workload... */
        nn_assert (count >= MESSAGES_PER_THREAD * TEST2_THREAD_COUNT);

        for (i = 0; i != TEST2_THREAD_COUNT; ++i) {
            nn_thread_term (&threads [i]);
        }
        nn_atomic_term (&active);
    }

    test_close (sb);

    return 0;
}
