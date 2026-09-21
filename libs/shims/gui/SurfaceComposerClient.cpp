#include <binder/IMemory.h>
#include <binder/IServiceManager.h>

// android::SurfaceComposerClient::unblankDisplay(android::sp<android::IBinder> const&)
extern "C" void _ZN7android21SurfaceComposerClient14unblankDisplayERKNS_2spINS_7IBinderEEE(const android::sp<android::IBinder>& display) {
    // no-op, the explicit function was replaced with setPowerMode()
}

// android::SurfaceComposerClient::blankDisplay(android::sp<android::IBinder> const&)
extern "C" void _ZN7android21SurfaceComposerClient12blankDisplayERKNS_2spINS_7IBinderEEE(const android::sp<android::IBinder>& display) {
    // no-op, the explicit function was replaced with setPowerMode()
}