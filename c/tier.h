#ifndef COLIBRI_TIER_H
#define COLIBRI_TIER_H

#include <stdint.h>

/* Pick one RAM/VRAM hot-store slot to replace from recent routing heat.
 * The fixed margin handles tiny samples; the 25% margin prevents ping-pong. */
static int tier_pick_swap(const uint32_t *heat, int nexpert,
                          const int *pinned, int npin,
                          int *slot, int *eid, long *gain){
    if(!heat || !pinned || npin<1 || nexpert<1) return 0;
    int cold=0;
    for(int z=1;z<npin;z++) if(heat[pinned[z]]<heat[pinned[cold]]) cold=z;
    int hot=-1; uint32_t fh=0;
    for(int e=0;e<nexpert;e++){
        int resident=0;
        for(int z=0;z<npin;z++) if(pinned[z]==e){ resident=1; break; }
        if(!resident && heat[e]>fh){ fh=heat[e]; hot=e; }
    }
    if(hot<0) return 0;
    uint32_t fc=heat[pinned[cold]];
    if(fh<=fc+(fc>>2)+4) return 0;
    *slot=cold; *eid=hot; *gain=(long)fh-(long)fc;
    return 1;
}

/* LFRU: frequency is the primary signal; recency breaks close calls. A recent
 * access contributes at most 255 points while one frequency count is worth
 * 256, so a merely recent expert cannot displace a genuinely hotter one. */
static uint64_t tier_lfru_score(uint32_t heat, uint32_t last, uint32_t clock){
    uint32_t age=clock-last, recent=age<255?255-age:0;
    return ((uint64_t)heat<<8)|recent;
}

static int tier_pick_lfru(const uint32_t *heat, const uint32_t *last, uint32_t clock,
                          int nexpert, const int *pinned, int npin,
                          int *slot, int *eid, long *gain){
    if(!heat||!last||!pinned||npin<1||nexpert<1) return 0;
    int cold=0;
    for(int z=1;z<npin;z++)
        if(tier_lfru_score(heat[pinned[z]],last[pinned[z]],clock)<
           tier_lfru_score(heat[pinned[cold]],last[pinned[cold]],clock)) cold=z;
    int hot=-1; uint64_t hs=0;
    for(int e=0;e<nexpert;e++){
        int resident=0; for(int z=0;z<npin;z++) if(pinned[z]==e){resident=1;break;}
        uint64_t score=tier_lfru_score(heat[e],last[e],clock);
        if(!resident&&(hot<0||score>hs)){ hot=e; hs=score; }
    }
    if(hot<0) return 0;
    uint64_t cs=tier_lfru_score(heat[pinned[cold]],last[pinned[cold]],clock);
    /* Retain the existing 25%+4-frequency hysteresis in score units. */
    if(hs<=cs+(cs>>2)+(4u<<8)) return 0;
    *slot=cold; *eid=hot; *gain=(long)((hs-cs)>>8); return 1;
}

/* Adaptive frequency protection for the streamed expert cache. Adapted from
 * PR #223's CLOCK-LRU-K policy: low-frequency residents compete by recency,
 * while proven reuse is protected until eviction-driven decay expires it. */
#define TIER_FMAX          15
#define TIER_K0             2
#define TIER_ADAPT_EVERY   16u
#define TIER_WIN_OPS       32u
#define TIER_RATE_LOW0   2500u
#define TIER_RATE_HIGH0  5000u
#define TIER_RATE_LOW_MIN   500u
#define TIER_RATE_LOW_MAX  4000u
#define TIER_RATE_HIGH_MIN 3000u
#define TIER_RATE_HIGH_MAX 8000u
#define TIER_RATE_STEP     1000u
#ifndef TIER_DECAY_EVERY
#define TIER_DECAY_EVERY    8u
#endif
#ifndef TIER_DECAY_OP
#define TIER_DECAY_OP(f)   ((f)-1)
#endif

typedef struct {
    int8_t k, last_dir;
    uint32_t ev_unprot, ev_prot, graduated, last_check;
    uint32_t rate_low, rate_high;
    uint32_t win_hits, win_ops, prev_hitrate;
    uint32_t decay_ctr;
} TierAdapt;

static void tier_adapt_init(TierAdapt *a){
    a->k=TIER_K0; a->last_dir=0;
    a->ev_unprot=a->ev_prot=a->graduated=a->last_check=0;
    a->rate_low=TIER_RATE_LOW0; a->rate_high=TIER_RATE_HIGH0;
    a->win_hits=a->win_ops=a->prev_hitrate=0;
    a->decay_ctr=0;
}

static void tier_probe(TierAdapt *a, int hit){
    a->win_ops++;
    if(hit) a->win_hits++;
}

static void tier_touch(TierAdapt *a, int8_t *freq, int at_cap){
    int8_t v=*freq;
    if(v<1 || v>=TIER_FMAX) return;
    if(v==a->k && at_cap) a->graduated++;
    *freq=(int8_t)(v+1);
}

static void tier_admit(int8_t *freq){
    int v=*freq;
    *freq=v<0 ? (int8_t)(-v+1>TIER_FMAX ? TIER_FMAX : -v+1) : (int8_t)1;
}

static void tier_evict(TierAdapt *a, int8_t *freq, int nexpert,
                       const uint32_t *last, int eid, int ghost_cap){
    if(freq[eid]>a->k) a->ev_prot++; else a->ev_unprot++;
    int ng=0, oldest=-1, first=1;
    uint32_t oldest_last=0;
    for(int e=0;e<nexpert;e++) if(e!=eid && freq[e]<0){
        ng++;
        if(first || last[e]<oldest_last){
            oldest=e; oldest_last=last[e]; first=0;
        }
    }
    if(ng>=ghost_cap && oldest>=0) freq[oldest]=0;
    if(freq[eid]>0) freq[eid]=(int8_t)-freq[eid];
    if(++a->decay_ctr>=TIER_DECAY_EVERY){
        a->decay_ctr=0;
        for(int e=0;e<nexpert;e++) if(freq[e]>1)
            freq[e]=(int8_t)TIER_DECAY_OP(freq[e]);
    }
}

static void tier_maybe_adapt(TierAdapt *a){
    uint32_t total=a->ev_unprot+a->ev_prot;
    if(total-a->last_check<TIER_ADAPT_EVERY) return;
    a->last_check=total;
    uint32_t graduated=a->graduated;
    if(!total){
        if(graduated>100) a->graduated=graduated/2;
        return;
    }
    if(a->win_ops<TIER_WIN_OPS) return;
    uint32_t hits=a->win_hits>a->win_ops ? a->win_ops : a->win_hits;
    uint32_t hitrate=(uint32_t)((uint64_t)hits*10000u/a->win_ops);
    if(a->prev_hitrate>0 && a->last_dir!=0){
        int improved=hitrate>a->prev_hitrate;
        if(a->last_dir>0){
            if(improved && a->rate_high>TIER_RATE_HIGH_MIN+TIER_RATE_STEP)
                a->rate_high-=TIER_RATE_STEP;
            else if(!improved && a->rate_high<TIER_RATE_HIGH_MAX-TIER_RATE_STEP)
                a->rate_high+=TIER_RATE_STEP;
        }else{
            if(improved && a->rate_low<TIER_RATE_LOW_MAX-TIER_RATE_STEP)
                a->rate_low+=TIER_RATE_STEP;
            else if(!improved && a->rate_low>TIER_RATE_LOW_MIN+TIER_RATE_STEP)
                a->rate_low-=TIER_RATE_STEP;
        }
    }
    a->prev_hitrate=hitrate; a->win_ops=0; a->win_hits=0;
    uint32_t rate=(uint32_t)((uint64_t)graduated*10000u/total);
    int8_t direction=0;
    if(rate<a->rate_low && a->k>1){ a->k--; direction=-1; }
    else if(rate>a->rate_high && a->k<TIER_FMAX-1){ a->k++; direction=1; }
    a->last_dir=direction;
    if(graduated>100) a->graduated=graduated/2;
    if(total>100){ a->ev_unprot/=2; a->ev_prot/=2; }
}

static void tier_decay(uint32_t *heat, int nexpert){
    for(int e=0;e<nexpert;e++) heat[e]>>=1;
}

#endif
