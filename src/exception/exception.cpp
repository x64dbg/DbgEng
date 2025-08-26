#include <cstdint>

int main()
{
    *(char*)(uintptr_t)0xDEADBEEF = 0;
}
