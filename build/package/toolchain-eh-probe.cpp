// Build-time probe only: unwind across a separate frame and run its destructor.
// It is never linked into the shipping AppImage runtime.
static volatile int destroyed = 0;

struct Guard {
    ~Guard() { ++destroyed; }
};

__attribute__((noinline)) static void unwind_one_frame()
{
    Guard guard;
    throw 37;
}

int main()
{
    try {
        unwind_one_frame();
    } catch (int value) {
        return value == 37 && destroyed == 1 ? 0 : 1;
    }
    return 2;
}
