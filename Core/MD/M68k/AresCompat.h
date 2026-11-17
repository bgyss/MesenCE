#pragma once
// Minimal stand-ins for the nall primitives used by the ares M68000 component
// (Core/MD/M68k/ares/*). Only the operations that code uses are provided.
// Natural<N> keeps nall's semantics: every store is masked to N bits and the
// value reads back as the smallest unsigned native type that holds N bits.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER) && !defined(__clang__)
	#define unreachable __assume(0)
#else
	#define unreachable __builtin_unreachable()
#endif

namespace AresCompat
{
	using u8 = uint8_t;
	using u16 = uint16_t;
	using u32 = uint32_t;
	using u64 = uint64_t;
	using s8 = int8_t;
	using s16 = int16_t;
	using s32 = int32_t;
	using s64 = int64_t;
	using i8 = int8_t;
	using i16 = int16_t;
	using i32 = int32_t;
	using i64 = int64_t;
	using b1 = bool;

	template<u32 Bits>
	using NaturalStorage = std::conditional_t<(Bits <= 8), u8, std::conditional_t<(Bits <= 16), u16, std::conditional_t<(Bits <= 32), u32, u64>>>;

	template<u32 Bits>
	struct Natural
	{
		using utype = NaturalStorage<Bits>;
		//if constexpr: the 64-bit case must not instantiate a 64-bit shift (MSVC C4293/C4310 under /WX)
		static constexpr utype MakeMask()
		{
			if constexpr(Bits >= 64) {
				return (utype)~0ull;
			} else {
				return (utype)((1ull << Bits) - 1);
			}
		}
		static constexpr utype Mask = MakeMask();

		utype data = 0;

		constexpr Natural() = default;
		template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>>>
		constexpr Natural(T value) : data((utype)((u64)value & Mask))
		{
		}
		template<u32 Other>
		constexpr Natural(Natural<Other> value) : data((utype)((u64)value.data & Mask))
		{
		}

		constexpr operator utype() const { return data; }

		template<typename T>
		Natural& operator=(T value)
		{
			data = (utype)((u64)value & Mask);
			return *this;
		}
		template<typename T>
		Natural& operator+=(T v) { return *this = (u64)data + (u64)v; }
		template<typename T>
		Natural& operator-=(T v) { return *this = (u64)data - (u64)v; }
		template<typename T>
		Natural& operator*=(T v) { return *this = (u64)data * (u64)v; }
		template<typename T>
		Natural& operator<<=(T v) { return *this = (u64)data << v; }
		template<typename T>
		Natural& operator>>=(T v) { return *this = (u64)data >> v; }
		template<typename T>
		Natural& operator|=(T v) { return *this = (u64)data | (u64)v; }
		template<typename T>
		Natural& operator&=(T v) { return *this = (u64)data & (u64)v; }
		template<typename T>
		Natural& operator^=(T v) { return *this = (u64)data ^ (u64)v; }
		Natural& operator++() { return *this = (u64)data + 1; }
		Natural& operator--() { return *this = (u64)data - 1; }
		Natural operator++(int)
		{
			Natural old = *this;
			++*this;
			return old;
		}
		Natural operator--(int)
		{
			Natural old = *this;
			--*this;
			return old;
		}

		struct BitRef
		{
			Natural& owner;
			u32 lo;
			u32 hi;
			operator utype() const { return (utype)((owner.data >> lo) & RangeMask()); }
			u64 RangeMask() const { return (hi - lo + 1) >= 64 ? ~0ull : ((1ull << (hi - lo + 1)) - 1); }
			template<typename T>
			BitRef& operator=(T value)
			{
				u64 m = RangeMask() << lo;
				owner = ((u64)owner.data & ~m) | (((u64)value << lo) & m);
				return *this;
			}
			template<typename T>
			BitRef& operator^=(T value) { return *this = (u64)(utype) * this ^ (u64)value; }
			template<typename T>
			BitRef& operator|=(T value) { return *this = (u64)(utype) * this | (u64)value; }
			template<typename T>
			BitRef& operator&=(T value) { return *this = (u64)(utype) * this & (u64)value; }
		};

		BitRef bit(u32 index) & { return BitRef { *this, index, index }; }
		BitRef bit(u32 lo, u32 hi) & { return BitRef { *this, lo, hi }; }
		BitRef byte(u32 index) & { return BitRef { *this, index * 8, index * 8 + 7 }; }
		utype bit(u32 index) const& { return (data >> index) & 1; }
		utype bit(u32 lo, u32 hi) const& { return (utype)((data >> lo) & (((hi - lo + 1) >= 64) ? ~0ull : ((1ull << (hi - lo + 1)) - 1))); }
		utype byte(u32 index) const& { return (utype)((data >> (index * 8)) & 0xff); }
	};

	using n1 = Natural<1>;
	using n3 = Natural<3>;
	using n4 = Natural<4>;
	using n8 = Natural<8>;
	using n12 = Natural<12>;
	using n16 = Natural<16>;
	using n24 = Natural<24>;
	using n32 = Natural<32>;
	using n64 = Natural<64>;

	struct RangeIter
	{
		u32 value;
		u32 operator*() const { return value; }
		RangeIter& operator++()
		{
			++value;
			return *this;
		}
		bool operator!=(const RangeIter& other) const { return value != other.value; }
	};
	struct Range
	{
		u32 count;
		RangeIter begin() const { return { 0 }; }
		RangeIter end() const { return { count }; }
	};
	inline Range range(u64 count)
	{
		return Range { (u32)count };
	}

	using std::swap;

	namespace bit
	{
		constexpr u32 count(u64 value)
		{
			u32 n = 0;
			for(; value; value &= value - 1) {
				n++;
			}
			return n;
		}

		// "1100 ---1 0000 ----": '1' is a set bit, every other non-space character is clear.
		constexpr u64 test(const char* s)
		{
			u64 result = 0;
			for(; *s; s++) {
				if(*s == ' ') {
					continue;
				}
				result = (result << 1) | (*s == '1' ? 1 : 0);
			}
			return result;
		}
	}

	// Subset of nall::string used by the disassembler: brace-initialised concatenation.
	struct string : std::string
	{
		string() = default;
		string(const char* s) : std::string(s) {}
		string(const std::string& s) : std::string(s) {}
		template<typename A, typename B, typename... Rest>
		string(A&& a, B&& b, Rest&&... rest)
		{
			Append(std::forward<A>(a));
			Append(std::forward<B>(b));
			(Append(std::forward<Rest>(rest)), ...);
		}

		template<typename... P>
		string& append(P&&... pieces)
		{
			(Append(std::forward<P>(pieces)), ...);
			return *this;
		}
		string& trimRight(const char* suffix)
		{
			size_t n = strlen(suffix);
			if(size() >= n && compare(size() - n, n, suffix) == 0) {
				erase(size() - n);
			}
			return *this;
		}
		explicit operator bool() const { return !empty(); }
		bool operator!() const { return empty(); }

	private:
		void Append(const char* s) { std::string::append(s); }
		void Append(const std::string& s) { std::string::append(s); }
		void Append(char c) { push_back(c); }
		template<u32 Bits>
		void Append(Natural<Bits> v) { std::string::append(std::to_string((u64)v.data)); }
		template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
		void Append(T v) { std::string::append(std::to_string(v)); }
	};

	// nall::pad: negative width pads on the right.
	inline string pad(const string& s, long width)
	{
		std::string out = s;
		long w = width < 0 ? -width : width;
		if((long)out.size() < w) {
			if(width < 0) {
				out.append(w - out.size(), ' ');
			} else {
				out.insert(0, w - out.size(), ' ');
			}
		}
		return string(out);
	}

	template<typename T>
	inline string hex(T value, long digits = 0, char fill = '0')
	{
		u64 v = (u64)value;
		static const char* chars = "0123456789abcdef";
		std::string out;
		do {
			out.insert(out.begin(), chars[v & 15]);
			v >>= 4;
		} while(v);
		if(digits > 0) {
			if((long)out.size() > digits) {
				out = out.substr(out.size() - digits);
			} else {
				out.insert(out.begin(), digits - out.size(), fill);
			}
		}
		return string(out);
	}
}
