#ifdef _DEBUG
	#define _ELPP_DEBUG_ERRORS
	#define _ELPP_DEBUG_ASSERT_FAILURE
	#define _ELPP_DISABLE_DEFAULT_CRASH_HANDLING
#endif

#define _ELPP_THREAD_SAFE
#define _ELPP_NO_DEFAULT_LOG_FILE
#define _ELPP_DISABLE_LOG_FILE_FROM_ARG
#define _ELPP_DISABLE_LOGGING_FLAGS_FROM_ARG
#define _ELPP_DISABLE_CUSTOM_FORMAT_SPECIFIERS

#include <easylogging++.h>