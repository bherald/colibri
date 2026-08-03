#include <stdio.h>
#include "../tier.h"

static int fail(const char *message){
    fprintf(stderr,"tier test failed: %s\n",message);
    return 1;
}

int main(void){
    uint32_t heat[6]={20,2,8,3,30,1};
    int pinned[2]={0,1}, slot=-1, eid=-1; long gain=0;
    if(!tier_pick_swap(heat,6,pinned,2,&slot,&eid,&gain)) return fail("hot expert not promoted");
    if(slot!=1 || eid!=4 || gain!=28) return fail("wrong promotion candidate");

    uint32_t stable[4]={20,18,24,4}; int resident[2]={0,1};
    if(tier_pick_swap(stable,4,resident,2,&slot,&eid,&gain)) return fail("hysteresis did not block churn");

    tier_decay(heat,6);
    if(heat[0]!=10 || heat[1]!=1 || heat[4]!=15) return fail("heat decay");

    uint32_t freq[5]={10,10,2,18,18}, last[5]={10,90,95,20,99};
    int live[2]={0,1};
    if(!tier_pick_lfru(freq,last,100,5,live,2,&slot,&eid,&gain)) return fail("LFRU promotion");
    if(slot!=0||eid!=4) return fail("LFRU did not prefer recent ties");

    TierAdapt adaptive; tier_adapt_init(&adaptive);
    if(adaptive.k!=TIER_K0) return fail("adaptive initial k");
    int8_t afreq[8]={0}; uint32_t routed_last[8]={0};
    tier_admit(&afreq[0]);
    if(afreq[0]!=1) return fail("fresh adaptive admission");
    for(int i=0;i<40;i++) tier_touch(&adaptive,&afreq[0],1);
    if(afreq[0]!=TIER_FMAX) return fail("adaptive frequency saturation");
    if(adaptive.graduated!=1) return fail("adaptive graduation counted more than once");
    routed_last[0]=7;
    tier_evict(&adaptive,afreq,8,routed_last,0,4);
    if(afreq[0]!=-TIER_FMAX) return fail("adaptive ghost memory");
    tier_admit(&afreq[0]);
    if(afreq[0]!=TIER_FMAX) return fail("adaptive ghost readmission");

    TierAdapt turnover; tier_adapt_init(&turnover);
    int8_t tfreq[5]={0,-3,-5,4,0}; uint32_t tlast[5]={0,10,12,2,0};
    tier_evict(&turnover,tfreq,5,tlast,3,2);
    if(tfreq[3]!=-4 || tfreq[1]!=0 || tfreq[2]!=-5)
        return fail("adaptive ghost turnover");

    TierAdapt decay; tier_adapt_init(&decay);
    int8_t dfreq[5]={0,8,TIER_FMAX,-6,1}; uint32_t dlast[5]={0};
    for(unsigned i=0;i<TIER_DECAY_EVERY;i++){
        dfreq[0]=1; tier_evict(&decay,dfreq,5,dlast,0,5); dfreq[0]=0;
    }
    if(dfreq[1]!=7 || dfreq[2]!=TIER_FMAX-1 || dfreq[3]!=-6 || dfreq[4]!=1)
        return fail("adaptive eviction-clock decay");

    TierAdapt low; tier_adapt_init(&low);
    int8_t lfreq[2]={0}; uint32_t llast[2]={0};
    for(unsigned i=0;i<TIER_WIN_OPS;i++) tier_probe(&low,0);
    for(unsigned i=0;i<TIER_ADAPT_EVERY;i++){
        lfreq[0]=1; tier_evict(&low,lfreq,2,llast,0,2); lfreq[0]=0;
        tier_maybe_adapt(&low);
    }
    if(low.k!=TIER_K0-1) return fail("adaptive k did not fall on zero reuse");
    puts("tier tests: ok");
    return 0;
}
