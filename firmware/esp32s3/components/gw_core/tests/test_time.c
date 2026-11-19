#include "gw_time_bounds.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    gw_time_anchor_t a={0};uint64_t lo,hi;
    assert(!gw_time_anchor_bounds(&a,0,&lo,&hi));
    assert(!gw_time_anchor_set(&a,1800000000000ULL,1001,0,1));
    assert(!gw_time_anchor_set(&a,1800000000000ULL,0,0,2001));
    assert(!gw_time_anchor_set(&a,UINT64_MAX,0,0,0));
    assert(!gw_time_anchor_set(&a,1800000000000ULL,0,2,1));
    assert(gw_time_anchor_set(&a,1800000000000ULL,1000,500,2500));
    assert(gw_time_anchor_bounds(&a,2500,&lo,&hi));
    assert(lo==1799999998997ULL&&hi==1800000003003ULL);
    assert(!gw_time_anchor_bounds(&a,2499,&lo,&hi));
    assert(gw_time_anchor_bounds(&a,59490,&lo,&hi));
    assert(!gw_time_anchor_bounds(&a,59492,&lo,&hi));
    assert(!gw_time_anchor_bounds(&a,UINT64_MAX,&lo,&hi));
    puts("PASS: signed-time interval RTT/drift/expiry/overflow/reset boundaries");
}
