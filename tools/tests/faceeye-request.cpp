#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
static void* create(void*) { return nullptr; }
static void destroy(void*) {}
static binder_status_t transact(AIBinder*,transaction_code_t,const AParcel*,AParcel*) { return STATUS_UNKNOWN_TRANSACTION; }
// Decoded from installed trackingfidelityservice_interfaces-V2-ndk.so:
// transaction 2: requestFeaturesWithFidelities(vector<FeatureFidelitySetting>, bool*).
// Verified against service dump: ORTHOFIT=2, FACE=3, EYE=4; HIGH=5, OFF=0.
static binder_status_t element(AParcel* p,const void* data,size_t i) {
 int level=*static_cast<const int*>(data);
 binder_status_t s=AParcel_writeInt32(p,1); // non-null element marker
 if (!s) s=AParcel_writeInt32(p,12); // sized parcelable: size, feature, level
 if (!s) s=AParcel_writeInt32(p,i==0?3:(i==1?4:2));
 if (!s) s=AParcel_writeInt32(p,i==2?0:level); // restore diagnostic orthofit request to OFF
 return s;
}
int main(int argc,char** argv) {
 int level=argc>1?atoi(argv[1]):5;
 if(level!=0 && level!=5) return 2;
 auto check=reinterpret_cast<AIBinder*(*)(const char*)>(dlsym(dlopen("libbinder_ndk.so",RTLD_NOW),"AServiceManager_checkService"));
 if(!check) return 3;
 AIBinder* b=check("oculus.internal.ITrackingFidelityService/default");
 if(!b) { puts("service unavailable"); return 3; }
 auto* c=AIBinder_Class_define("oculus.internal.ITrackingFidelityService",create,destroy,transact);
 if(!AIBinder_associateClass(b,c)) return 4;
 AParcel *in=nullptr,*out=nullptr;
 binder_status_t s=AIBinder_prepareTransaction(b,&in);
 if(!s) s=AParcel_writeParcelableArray(in,&level,3,element);
 if(!s) s=AIBinder_transact(b,2,&in,&out,0);
 if(s) { printf("transaction error %d\n",s); return 5; }
 AStatus* status=nullptr;
 s=AParcel_readStatusHeader(out,&status);
 if(s || !status || !AStatus_isOk(status)) {
  printf("service error %d: %s\n",s,status?AStatus_getMessage(status):"no status"); return 6;
 }
 bool accepted=false; s=AParcel_readBool(out,&accepted);
 printf("ET/FT fidelity=%d accepted=%d readStatus=%d\n",level,accepted,s);
 AStatus_delete(status); AParcel_delete(out); AIBinder_decStrong(b);
 return s || !accepted;
}
