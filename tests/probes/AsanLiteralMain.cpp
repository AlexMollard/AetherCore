#include <cstdio>

extern "C" const char* AsanNarrowLiteral();
extern "C" const wchar_t* AsanWideLiteral();

int main()
{
	const auto* narrow = AsanNarrowLiteral();
	const auto* wide = AsanWideLiteral();
	std::puts("ASAN cross-TU narrow/wide literal registration reached main");
	return narrow[0] == '.' && narrow[1] == '\0' && wide[0] == L'.' && wide[1] == L'\0' ? 0 : 2;
}
