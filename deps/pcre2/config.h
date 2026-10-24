/* tphp standalone pcre2 config — derived from php-src 8.5.7 ext/pcre/pcre2lib
 * config.h with the PHP build-system includes stripped. v1: no JIT
 * (SUPPORT_JIT left undefined; sources still compile). */

#define SUPPORT_UNICODE 1
#define SUPPORT_PCRE2_8 1

#if defined(__GNUC__) && __GNUC__ >= 4
# ifdef __cplusplus
#  define PCRE2_EXP_DECL		extern "C" __attribute__ ((visibility("default")))
# else
#  define PCRE2_EXP_DECL		extern __attribute__ ((visibility("default")))
# endif
# define PCRE2_EXP_DEFN		__attribute__ ((visibility("default")))
#endif

/* This limits the amount of memory that pcre2_match() may use while matching
   a pattern. The value is in kilobytes. */
#ifndef HEAP_LIMIT
#define HEAP_LIMIT 20000000
#endif

/* The value of PARENS_NEST_LIMIT specifies the maximum depth of nested
   parentheses (of any kind) in a pattern. */
#ifndef PARENS_NEST_LIMIT
#define PARENS_NEST_LIMIT 250
#endif

#ifndef MATCH_LIMIT
#define MATCH_LIMIT 10000000
#endif

#ifndef MATCH_LIMIT_DEPTH
#define MATCH_LIMIT_DEPTH MATCH_LIMIT
#endif

#ifndef MAX_NAME_COUNT
#define MAX_NAME_COUNT 10000
#endif

#ifndef MAX_NAME_SIZE
#define MAX_NAME_SIZE 128
#endif

#ifndef NEWLINE_DEFAULT
#define NEWLINE_DEFAULT 2
#endif

#ifndef LINK_SIZE
#define LINK_SIZE 2
#endif

#ifndef MAX_VARLOOKBEHIND
#define MAX_VARLOOKBEHIND 255
#endif

/* to make a symbol visible */
#ifndef PCRE2_EXPORT
#define PCRE2_EXPORT
#endif
