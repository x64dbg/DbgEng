#include <cstdlib>
#include <cstdio>
#include <cstdint>

#include <DbgEng.h>

int main(int argc, char** argv)
{
    IDebugClient9* debugClient = nullptr;
    if (const auto result = DebugCreate(__uuidof(IDebugClient8), reinterpret_cast<void**>(&debugClient));
        result != S_OK)
    {
        printf("Failed to create IDebugClient9: %08x\n", (uint32_t)result);
        return EXIT_FAILURE;
    }
    printf("Created IDebugClient9\n");
    debugClient->Release();
    return EXIT_SUCCESS;
}
