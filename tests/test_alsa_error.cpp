#include "io/alsa/AlsaError.h"
#include "TestHarness.h"

using namespace rt;

void testSuccessIsNotAnError() {
    CHECK(alsaActionFor(0) == AlsaAction::None);
    CHECK(alsaActionFor(1200) == AlsaAction::None);
}

void testTransientCodesRetry() {
    CHECK(alsaActionFor(-EINTR)  == AlsaAction::Retry);
    CHECK(alsaActionFor(-EAGAIN) == AlsaAction::Retry);
}

void testXrunAndSuspendRecover() {
    CHECK(alsaActionFor(-EPIPE)    == AlsaAction::Recover);
    CHECK(alsaActionFor(-ESTRPIPE) == AlsaAction::Recover);
}

void testUnmappedCodesFail() {
    CHECK(alsaActionFor(-ENODEV) == AlsaAction::Fail);
    CHECK(alsaActionFor(-12345)  == AlsaAction::Fail);
}

int main() {
    RUN(testSuccessIsNotAnError);
    RUN(testTransientCodesRetry);
    RUN(testXrunAndSuspendRecover);
    RUN(testUnmappedCodesFail);
    TEST_MAIN_END
}
