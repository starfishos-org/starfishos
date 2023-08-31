#include <float.h>
#define C(m,s) (m==LDBL_MANT_DIG && s==sizeof(long double))
typedef char ldcheck[(C(53,8)||C(64,12)||C(64,16)||C(113,16))*2-1];
