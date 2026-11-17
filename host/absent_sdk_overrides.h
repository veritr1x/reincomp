#pragma once
// The APK's Java SDKs (Adjust analytics here) are not part of the translated
// libraries, and the JNI host does not run Java code. Their C# wrappers
// therefore raise AndroidJavaException, which aborts the caller's async flow
// (e.g. the Terms of Service step after "Yes"). These overrides make each
// wrapper a no-op that returns 0/false/null, as if the SDK were disabled.
// Addresses: global 3.7.1 libil2cpp (SHA-pinned).
#include <atomic>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "../runtime/runtime.h"

namespace bunker::windows::absent_sdks {

// Synchronous guest call from a host bridge on the current guest thread
// (Pthreads::Run). Set by the bootstrap once the thread host exists.
using GuestCall=std::function<uint64_t(addr_t,const std::vector<addr_t>&)>;
inline GuestCall &Call(){static GuestCall call;return call;}

namespace detail {
inline uint64_t Read64(addr_t a){uint64_t v;std::memcpy(&v,reinterpret_cast<const void*>(a),8);return v;}
inline uint32_t Read32(addr_t a){uint32_t v;std::memcpy(&v,reinterpret_cast<const void*>(a),4);return v;}
}  // namespace detail

// Register before recomp::PublishGuestOverrides().
inline void AddOverrides(){
  static const struct {addr_t entry;const char *name;} kNoOps[]={
      {0x83fe1fc0,"Adjust$$Awake"},
      {0x83fe22dc,"Adjust$$start"},
      {0x83fe2350,"Adjust$$OnApplicationPause"},
      {0x83fe23d0,"AdjustAndroid$$OnPause"},
      {0x83fe24e8,"AdjustAndroid$$OnResume"},
      {0x83fe2600,"AdjustAndroid$$Start"},
      {0x83fe3810,"Adjust$$trackEvent"},
      {0x83fe3884,"AdjustAndroid$$TrackEvent"},
      {0x83fe3d88,"Adjust$$setEnabled"},
      {0x83fe3dec,"AdjustAndroid$$SetEnabled"},
      {0x83fe3ef8,"Adjust$$isEnabled"},
      {0x83fe3f54,"AdjustAndroid$$IsEnabled"},
      {0x83fe4074,"Adjust$$setOfflineMode"},
      {0x83fe40d8,"AdjustAndroid$$SetOfflineMode"},
      {0x83fe41e4,"Adjust$$setDeviceToken"},
      {0x83fe4248,"AdjustAndroid$$SetDeviceToken"},
      {0x83fe4368,"Adjust$$gdprForgetMe"},
      {0x83fe43c4,"AdjustAndroid$$GdprForgetMe"},
      {0x83fe44b8,"Adjust$$disableThirdPartySharing"},
      {0x83fe4514,"AdjustAndroid$$DisableThirdPartySharing"},
      {0x83fe4608,"Adjust$$appWillOpenUrl"},
      {0x83fe466c,"AdjustAndroid$$AppWillOpenUrl"},
      {0x83fe4824,"Adjust$$sendFirstPackages"},
      {0x83fe4880,"AdjustAndroid$$SendFirstPackages"},
      {0x83fe4998,"Adjust$$addSessionPartnerParameter"},
      {0x83fe4a0c,"AdjustAndroid$$AddSessionPartnerParameter"},
      {0x83fe4b94,"Adjust$$addSessionCallbackParameter"},
      {0x83fe4c08,"AdjustAndroid$$AddSessionCallbackParameter"},
      {0x83fe4d90,"Adjust$$removeSessionPartnerParameter"},
      {0x83fe4df4,"AdjustAndroid$$RemoveSessionPartnerParameter"},
      {0x83fe4f50,"Adjust$$removeSessionCallbackParameter"},
      {0x83fe4fb4,"AdjustAndroid$$RemoveSessionCallbackParameter"},
      {0x83fe5110,"Adjust$$resetSessionPartnerParameters"},
      {0x83fe516c,"AdjustAndroid$$ResetSessionPartnerParameters"},
      {0x83fe52f4,"Adjust$$resetSessionCallbackParameters"},
      {0x83fe5350,"AdjustAndroid$$ResetSessionCallbackParameters"},
      {0x83fe54d8,"Adjust$$trackAdRevenue"},
      {0x83fe554c,"AdjustAndroid$$TrackAdRevenue"},
      {0x83fe5744,"Adjust$$trackAppStoreSubscription"},
      {0x83fe5748,"Adjust$$trackPlayStoreSubscription"},
      {0x83fe57ac,"AdjustAndroid$$TrackPlayStoreSubscription"},
      {0x83fe5cf4,"Adjust$$trackThirdPartySharing"},
      {0x83fe5d58,"AdjustAndroid$$TrackThirdPartySharing"},
      {0x83fe61d8,"Adjust$$trackMeasurementConsent"},
      {0x83fe623c,"AdjustAndroid$$TrackMeasurementConsent"},
      {0x83fe6348,"Adjust$$requestTrackingAuthorizationWithCompletionHandler"},
      {0x83fe634c,"Adjust$$updateConversionValue"},
      {0x83fe6350,"Adjust$$getAppTrackingAuthorizationStatus"},
      {0x83fe6358,"Adjust$$getAdid"},
      {0x83fe63b4,"AdjustAndroid$$GetAdid"},
      {0x83fe64d4,"Adjust$$getAttribution"},
      {0x83fe6530,"AdjustAndroid$$GetAttribution"},
      {0x83fe6e34,"Adjust$$getWinAdid"},
      {0x83fe6e84,"Adjust$$getIdfa"},
      {0x83fe6ed4,"Adjust$$getSdkVersion"},
      {0x83fe6f30,"AdjustAndroid$$GetSdkVersion"},
      {0x83fe706c,"Adjust$$setReferrer"},
      {0x83fe70d0,"AdjustAndroid$$SetReferrer"},
      {0x83fe71f0,"Adjust$$getGoogleAdId"},
      {0x83fe7254,"AdjustAndroid$$GetGoogleAdId"},
      {0x83fe7390,"Adjust$$getAmazonAdId"},
      {0x83fe73ec,"AdjustAndroid$$GetAmazonAdId"},
      {0x83fe74e8,"Adjust$$SetTestOptions"},
      {0x83fe754c,"AdjustAndroid$$SetTestOptions"},
      {0x83fe7750,"AdjustAndroid$$IsAppSecretSet"},
      // Game-side helpers that build com.adjust.sdk.AdjustEvent directly.
      {0x836175ec,"AdjustUtility$$Initialize"},
      {0x83617a14,"AdjustUtility$$TrackEvent"},
      {0x83617b74,"AdjustUtility$$TrackPurchaseEvent"},
      {0x83617bf4,"AdjustUtility$$TrackPurchaseRevenueEvent"},
      {0x83617c54,"AdjustUtility$$TrackStoryEvent"},
      {0x83617ddc,"AdjustUtility$$TrackQuestClearEvent"},
      {0x83617f64,"AdjustUtility$$TrackDownloadEvent"},
      {0x836180ec,"AdjustUtility$$PlayerLevelUpEvent"},
      // com.nekolaboratory.Seeker device probes (UserAuthComposite.DoAuth):
      // false/null reports, which TrList.AddTrJson accepts.
      {0x83401d74,"DeviceUtil$$GetIjb"},
      {0x83401d7c,"DeviceUtil$$GetHig"},
      {0x83401d84,"DeviceUtil$$GetAcs"},
      {0x83401fe0,"DeviceUtil$$GetPer"},
      {0x83402234,"DeviceUtil$$GetImu"},
      {0x83402488,"DeviceUtil$$GetIr"},
      {0x834026dc,"DeviceUtil$$GetIda"},
      {0x83402930,"DeviceUtil$$GetMsl"},
      {0x834029c8,"DeviceUtil$$GetIcs"},
      // Google Play Integrity: the manager wraps a Play Core Java object.
      {0x853f7b94,"IntegrityManager$$.ctor"},
      // Firebase Analytics: the desktop FirebaseCppApp has no analytics
      // exports, so FirebaseAnalyticsPINVOKE's type initializer throws and
      // aborts the title flow after the ad-tracking choice. This is the only
      // FirebaseAnalytics entry point the stripped build keeps.
      {0x853e84a0,"FirebaseAnalytics$$SetAnalyticsCollectionEnabled"},
  };
  for(const auto &row:kNoOps){
    auto hits=std::make_shared<std::atomic<uint32_t>>(0);const char *name=row.name;
    recomp::AddGuestOverride(row.entry,[hits,name](State &s){
      if(hits->fetch_add(1)<4)std::fprintf(stderr,"Absent SDKs: %s skipped\n",name);
      s.gpr.x0.qword=0;});
  }
  std::fprintf(stderr,"Absent SDKs: %zu absent SDK entries are no-ops\n",sizeof(kNoOps)/sizeof(kNoOps[0]));

  // Lilium (com.nekolaboratory.Lilium) device attestation, used by
  // SafetyNet.Lilium(apiKey, nonce) during user auth. AttestListener is an
  // AndroidJavaProxy for a Java interface that does not exist here, so its
  // constructor only stores the callback (field 0x20), and Attest completes
  // at once by calling listener.onResult(nonce) in guest code.
  recomp::AddGuestOverride(0x83615c94,[](State &s){  // SafetyNet.AttestListener..ctor
    const uint64_t callback=s.gpr.x1.qword;
    recomp::WriteGuestMemory(s.gpr.x0.qword+0x20,&callback,sizeof(callback));});
  recomp::AddGuestRedirect(0x83615d1c,[](State &s)->addr_t{  // SafetyNet.Attest(apiKey, nonce, listener)
    std::fprintf(stderr,"Absent SDKs: Lilium attestation answered locally\n");
    s.gpr.x0.qword=s.gpr.x2.qword;s.gpr.x2.qword=0;  // onResult(this=listener, response=nonce)
    return 0x8361614c;});

  // IntegrityManager.RequestIntegrityToken(request): Play services are absent,
  // so return a real IntegrityAsyncOperation<IntegrityTokenResponse> that is
  // already done with an error (IsSuccessful false); DoAuth then omits the
  // token. Mirrors the original prologue: method metadata initialization
  // (0x879a4e5f flag), object allocation and the generic constructor.
  recomp::AddGuestOverride(0x853f7ea8,[](State &s){
    using detail::Read32;using detail::Read64;
    auto &call=Call();
    if(!call){std::fprintf(stderr,"Absent SDKs: no guest call runner for Play Integrity\n");std::abort();}
    constexpr addr_t kInitFlag=0x879a4e5f;
    if(!(*reinterpret_cast<const volatile uint8_t*>(kInitFlag)&1)){
      call(0x825c6f04,{Read32(Read64(0x8726c518))});
      const uint8_t one=1;recomp::WriteGuestMemory(kInitFlag,&one,1);
    }
    const addr_t operation=call(0x825c6fcc,{Read64(Read64(0x8734eed8))});
    if(!operation){std::fprintf(stderr,"Absent SDKs: Play Integrity operation allocation failed\n");std::abort();}
    call(0x8382d9dc,{operation,Read64(Read64(0x87356ae0))});
    const uint8_t done=1;const int32_t api_not_available=-1;
    recomp::WriteGuestMemory(operation+0x14,&api_not_available,4);  // Error
    recomp::WriteGuestMemory(operation+0x10,&done,1);                // IsDone
    std::fprintf(stderr,"Absent SDKs: Play Integrity answered with API_NOT_AVAILABLE\n");
    s.gpr.x0.qword=operation;});
}

}  // namespace bunker::windows::absent_sdks
