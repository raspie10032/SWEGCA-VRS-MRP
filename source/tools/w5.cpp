// Crash driver: source files are read-only; all writes go below supplied work dir.
#include "vrs.h"
#include "cc_io.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    try {
        const std::string mode=argv[1], src=argv[2], dest=argv[3], idx=argv[4];
        std::vector<std::string> rels;
        { std::ifstream f(argv[5]); for(std::string s;std::getline(f,s);) if(!s.empty()) rels.push_back(s); }
        if(rels.size()!=3) return 2;
        std::filesystem::create_directories(dest); std::filesystem::create_directories(idx);
        for(auto& r:rels) std::filesystem::create_directories(std::filesystem::path(dest+"/"+r).parent_path());
        unsigned cpus[12]; for(unsigned i=0;i<12;i++) cpus[i]=i;
        vrs_opts o{}; o.sessions_dir=dest.c_str();o.index_dir=idx.c_str();o.workers=12;o.cpus=cpus;o.ncpus=12;o.segwin=1;
        vrs_h* h=nullptr; int rc=vrs_open(&o,&h); if(rc) return 3;
        if(vrs_wait(h,nullptr,30000)) return 4;
        std::puts("READY");std::fflush(stdout);
        if(mode!="reopen") {
            std::vector<std::ifstream> inputs(3);std::vector<std::unique_ptr<cc::File>> outputs;
            std::vector<uint64_t> offs;
            for(size_t i=0;i<3;i++) {
                outputs.emplace_back(new cc::File(dest+"/"+rels[i],true));
                offs.push_back(outputs.back()->size());
                inputs[i].open(src+"/"+rels[i],std::ios::binary);inputs[i].seekg(offs.back());
                if(!inputs[i]) return 5;
            }
            for(bool any=true;any;) {
                any=false;
                for(size_t i=0;i<3;i++) {
                    std::string line;if(!std::getline(inputs[i],line)) continue;any=true;
                    const std::string p=dest+"/"+rels[i]; outputs[i]->write_once(line+'\n',offs[i]);
                    int ins=vrs_insert(h,p.c_str(),offs[i],line.data(),line.size());
                    int wr=vrs_written(h,p.c_str(),offs[i]);
                    if(ins<0 || (wr!=0 && wr!=-3)) {std::printf("ERROR insert=%d written=%d\n",ins,wr);return 6;}
                    offs[i]+=line.size()+1;
                    std::printf("LINE %zu %llu\n",i,(unsigned long long)offs[i]);std::fflush(stdout);
                    if(mode=="append") std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            }
            for(size_t i=0;i<3;i++) if(vrs_wait_off(h,(dest+"/"+rels[i]).c_str(),offs[i],30000)) return 7;
        }
        // Wait for closed-turn contribution work as well as the published input prefix.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        vrs_dbg d{};
        do {vrs_dbg_stats(h,&d);if(!d.pend && !d.sweep_left) break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        while(std::chrono::steady_clock::now()<deadline);
        if(d.pend||d.sweep_left) return 8;
        std::printf("STATS %llu %llu %llu %llu %llu %llu %llu\n",(unsigned long long)d.cells_hash,(unsigned long long)d.cells_n,
            (unsigned long long)d.items,(unsigned long long)d.turns,(unsigned long long)d.sessions,
            (unsigned long long)d.shards_loaded,(unsigned long long)d.shards_rebuilt);
        vrs_close(h); return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 9;}
}
