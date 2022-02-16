// #include <stdio.h>
// #include "../include/common/region.h"

// metafs::region_map_t region_map;

// int main() {
//     int range = 10; // 0-999
//     // 左闭右闭区间
//     for(int i = 0; i < range; i++) {
//         region_map.insert(std::pair<metafs::ServerRegion*, metafs::region_id_t>(
//                         new metafs::ServerRegion(i, metafs::RegionKey(0, i*10), metafs::RegionKey(0, (i+1) * 10 - 1)), i));
//     }

//     auto iter = region_map.begin();
//     while(iter != region_map.end()) {
//         printf("region#%d, start:%d, end:%d\n", iter->second, iter->first->start_key.hi, iter->first->end_key.hi);
//         iter++;
//     }
// }