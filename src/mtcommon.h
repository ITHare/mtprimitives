#ifndef ithare_mtprimitives_mtcommon_h_included
#define ithare_mtprimitives_mtcommon_h_included

namespace ithare {
	namespace mtprimitives {
		constexpr inline bool mt_is_powerof2(size_t v) {//from https://stackoverflow.com/questions/10585450/how-do-i-check-if-a-template-parameter-is-a-power-of-two
			return v && ((v & (v - 1)) == 0);
		}
	}
}

#endif//ithare_mtprimitives_mtcommon_h_included

