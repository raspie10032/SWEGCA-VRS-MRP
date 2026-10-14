#pragma once
// One scalar core source, two compilation targets. Device-side fixed arrays
// preserve the host std::array ABI; runtime checks verify all transferred sizes.
#if defined(__CUDACC_RTC__)
namespace swegca::platform {
using size_t=unsigned long;using uint8_t=unsigned char;using uint32_t=unsigned int;using uint64_t=unsigned long;
enum class byte:unsigned char {};
template<class T,size_t N>struct array{T values[N];constexpr T& operator[](size_t n){return values[n];}constexpr const T& operator[](size_t n)const{return values[n];}constexpr void fill(T v){for(size_t i=0;i<N;++i)values[i]=v;}};
inline bool isfinite(double x){return ::isfinite(x);}inline double sqrt(double x){return ::sqrt(x);}
inline double fmin(double a,double b){return ::fmin(a,b);}inline double fmax(double a,double b){return ::fmax(a,b);}inline double fabs(double x){return ::fabs(x);}
}
static_assert(sizeof(double)==8&&sizeof(unsigned long)==8);
#else
#include "swegca_architecture/numeric_contract.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
namespace swegca::platform {
using std::size_t;using std::uint8_t;using std::uint32_t;using std::uint64_t;using std::byte;
template<class T,size_t N>using array=std::array<T,N>;
using std::isfinite;using std::sqrt;using std::fmin;using std::fmax;using std::fabs;
}
#endif
