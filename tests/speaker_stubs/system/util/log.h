#ifndef RETROTUNER_TEST_SPEAKER_LOG_H
#define RETROTUNER_TEST_SPEAKER_LOG_H
/* Keep hardware logging out of this narrowly scoped host test. */
#define DEF_LOG_RESULT(function, success, result) ((void)(result))
#endif
