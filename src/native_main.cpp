#if defined(ENV_NATIVE_BUILD) && !defined(PIO_UNIT_TESTING)

// The native environment primarily hosts Unity tests.  Keeping a tiny main
// also makes a deliberate `pio run -e native` a valid configuration check.
int main() {
    return 0;
}

#endif
