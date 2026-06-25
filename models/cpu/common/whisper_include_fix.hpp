// Include this before any header that pulls in whisper (Args.hpp, Session.hpp).
// On Linux, <unistd.h> etc. define macro "linux" which conflicts with whisper's variable name.
#ifdef linux
#undef linux
#endif
