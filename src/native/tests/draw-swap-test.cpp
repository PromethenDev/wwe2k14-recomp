// Host-only U9/astra1 test. No SDK, GPU, guest memory, or game executable.
#include "../nr/nr_draw_swap.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <array>
#include <stdexcept>
using namespace nr::backend;
using namespace nr::backend::draw_swap;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
static const std::vector<uint8_t> vs{1,2,3}, ps{4,5,6};
static uint64_t JobValue(uint32_t seq) { return uint64_t(seq) * 0x123456789ull; }
static void Fill(Draw& d, uint32_t seq, uint64_t* jobs = nullptr, bool jobs_only = false) {
  d.Reset();
  d.color=seq+7; d.depth=seq+9; d.vs_key=seq*19ull; d.ps_key=seq*31ull;
  d.vs_spirv=&vs; d.ps_spirv=seq%5 ? &ps : nullptr; d.spec_constants=seq^0x1234;
  d.sample_shading=seq&1; d.topology=seq%6; d.cull=seq%3; d.front_ccw=seq&2; d.depth_clamp=seq&4;
  for (uint32_t i=0;i<seq%5;++i) {
    d.attributes.push_back({i,VtxFormat::kR32G32_UINT,i,seq+i});
    VertexBuffer b; b.stride_bytes=16+i*4; b.prefix_bytes=seq%16;
    b.cache_id=seq%3 ? seq+i : 0; b.cache_size=seq+100; b.cache_upload=seq&1;
    b.data.resize((seq%3 ? 0 : 17+seq%300)+i);
    for (size_t k=0;k<b.data.size();++k) b.data[k]=uint8_t(seq+k+i);
    d.buffers.push_back(std::move(b));
  }
  d.indices.resize(seq%7*4);
  for (size_t i=0;i<d.indices.size();++i) d.indices[i]=uint8_t(seq+i*3);
  d.index32=seq&1; d.count=seq+1;
  for (size_t i=0;i<4096;++i) { d.vs_constants[i]=uint8_t(seq+i); d.ps_constants[i]=uint8_t(seq-i); }
  for (size_t i=0;i<1024;++i) d.shared[i]=uint8_t(seq^i);
  for (uint32_t i=0;i<seq%4;++i) {
    TextureBinding b{}; b.slot=i; b.dimension=i%3; b.texture=seq+i;
    b.sampler.mag_linear=seq&1; b.sampler.min_linear=seq&2; b.sampler.mip_linear=seq&4;
    b.sampler.address_u=seq%5; b.sampler.address_v=(seq+1)%5; b.sampler.address_w=(seq+2)%5;
    b.sampler.border=seq%3; b.sampler.max_anisotropy=seq%16;
    uint32_t nan=0x7fc00000u|seq; std::memcpy(&b.sampler.min_lod,&nan,4);
    b.sampler.max_lod=float(seq); b.sampler.lod_bias=-0.0f;
    d.textures.push_back(b);
  }
  for (unsigned i=0;i<6;++i) d.viewport[i]=float(seq+i);
  for (unsigned i=0;i<4;++i) { d.scissor[i]=int32_t(seq)-int32_t(i); d.blend_constants[i]=float(seq-i); }
  d.depth_test=seq&1; d.depth_write=seq&2; d.depth_compare=seq%8+1; d.stencil=seq&4;
  d.stencil_ref=seq; d.stencil_read=seq^255; d.stencil_write=seq^127;
  d.stencil_front={seq,seq+1,seq+2,seq+3}; d.stencil_back={seq+4,seq+5,seq+6,seq+7};
  d.blend={bool(seq&1),seq,seq+1,seq+2,seq+3,seq+4,seq+5,uint8_t(seq)};
  d.ordinal=seq; d.pre_ops={seq,seq+1}; d.pre_job_hashes={JobValue(seq)};
  d.pre_job_names={"test_job"};
  // Large closure forces std::function heap ownership, copied in CHECK but executed only on the consumer.
  std::array<uint64_t,32> captured{}; captured.fill(JobValue(seq));
  d.pre_jobs.push_back([jobs,captured] { if (jobs) *jobs += captured[17]; });
  d.jobs_only=jobs_only; d.index_cache_id=seq%13; d.index_cache_bytes=seq*4;
  d.index_cache_upload=seq&1; d.cache_flush=seq%17==0;
}
static void ComparatorTest() {
  Draw a; Fill(a,23); Draw b=a; REQUIRE(SameOutput(a,b));
#define MUTATE(expr) do { b=a; expr; REQUIRE(!SameOutput(a,b)); } while (0)
  MUTATE(++b.color); MUTATE(++b.depth); MUTATE(++b.vs_key); MUTATE(++b.ps_key);
  MUTATE(b.vs_spirv=nullptr); MUTATE(b.ps_spirv=nullptr); MUTATE(++b.spec_constants);
  MUTATE(b.sample_shading=!b.sample_shading); MUTATE(++b.topology); MUTATE(++b.cull);
  MUTATE(b.front_ccw=!b.front_ccw); MUTATE(b.depth_clamp=!b.depth_clamp);
  MUTATE(++b.attributes[0].location); MUTATE(b.attributes[0].format=VtxFormat::kUnknown);
  MUTATE(++b.attributes[0].binding); MUTATE(++b.attributes[0].offset_bytes);
  MUTATE(++b.buffers[0].stride_bytes); MUTATE(++b.buffers[0].prefix_bytes);
  MUTATE(++b.buffers[0].cache_id); MUTATE(++b.buffers[0].cache_size); MUTATE(b.buffers[0].cache_upload=!b.buffers[0].cache_upload);
  MUTATE(b.buffers[0].data.push_back(7)); MUTATE(++b.indices[0]); MUTATE(b.index32=!b.index32); MUTATE(++b.count);
  MUTATE(++b.vs_constants[4095]); MUTATE(++b.ps_constants[4095]); MUTATE(++b.shared[1023]);
  MUTATE(++b.textures[0].slot); MUTATE(++b.textures[0].dimension); MUTATE(++b.textures[0].texture);
  MUTATE(b.textures[0].sampler.lod_bias=0.0f); MUTATE(b.textures[0].sampler.min_lod=0.0f);
  MUTATE(++b.viewport[5]); MUTATE(++b.scissor[3]); MUTATE(++b.blend_constants[3]);
  MUTATE(b.depth_test=!b.depth_test); MUTATE(b.depth_write=!b.depth_write); MUTATE(++b.depth_compare);
  MUTATE(b.stencil=!b.stencil); MUTATE(++b.stencil_ref); MUTATE(++b.stencil_read); MUTATE(++b.stencil_write);
  MUTATE(++b.stencil_front.fail); MUTATE(++b.stencil_back.compare); MUTATE(++b.blend.src);
  MUTATE(++b.blend.write_mask); MUTATE(++b.ordinal); MUTATE(++b.pre_ops[0]); MUTATE(++b.pre_job_hashes[0]);
  MUTATE(b.pre_job_names[0]="changed"); MUTATE(b.pre_jobs.clear()); MUTATE(b.jobs_only=!b.jobs_only);
  MUTATE(++b.index_cache_id); MUTATE(++b.index_cache_bytes); MUTATE(b.index_cache_upload=!b.index_cache_upload);
  MUTATE(b.cache_flush=!b.cache_flush);
#undef MUTATE
  REQUIRE(ParseMode(nullptr)==0); REQUIRE(ParseMode("")==0); REQUIRE(ParseMode("0")==0);
  REQUIRE(ParseMode("1")==1); REQUIRE(ParseMode("2")==2); REQUIRE(ParseMode("10")==0); REQUIRE(ParseMode("2x")==0);
  std::cout<<"PASS comparator mutations, NaN/signed-zero bytes, strict switch parsing\n";
}
static void Stress(int mode, uint32_t threshold) {
  constexpr uint32_t n=12000;
  uint64_t jobs=0, expected_jobs=0, consumed=0, mismatches=0, checked=0, swapped=0;
  std::atomic<bool> first{true};
  nr::RecordQueue<Draw,128> q([&](Draw& d) {
    if (first.exchange(false)) std::this_thread::sleep_for(std::chrono::milliseconds(15));
    Draw expected; Fill(expected,d.ordinal,nullptr,d.jobs_only);
    if (!SameOutput(d,expected) || d.ordinal != consumed+1) ++mismatches;
    for (auto& job:d.pre_jobs) job();
    d.pre_jobs.clear(); // mirror RecordOne cleanup
    ++consumed;
  });
  auto owner=std::make_unique<Draw>();
  auto wrong=std::make_unique<Draw>();
  Draw* slot=q.Acquire(); Draw* old=owner.get();
  REQUIRE(!q.ExchangeAcquired(wrong.get(),owner)); REQUIRE(owner.get()==old);
  std::unique_ptr<Draw> empty;
  REQUIRE(!q.ExchangeAcquired(slot,empty)); REQUIRE(!Handoff(q,slot,owner,0).swapped);
  for (uint32_t seq=1;seq<=n;++seq) {
    // Abandoned private draws are never published; their jobs must never run.
    if (seq%71==0) { Fill(*owner,0xfeed,&jobs); owner->Reset(); }
    Fill(*owner,seq,&jobs);
    // Private in-progress data must survive jobs-only enqueue/drain before submission.
    if (seq>256 && seq%37==0) {
      Draw saved=*owner;
      q.Sync(); REQUIRE(SameOutput(saved,*owner));
    }
    const bool jobs_only=seq%19==0;
    slot=q.Acquire();
    if (jobs_only) { // mirrors FlushPendingRecordOps: bypass private-draw exchange
      Fill(*slot,seq,&jobs,true);
      q.SubmitBatch(false,threshold);
    } else {
      Draw* identity=owner.get(); const void* closures=owner->pre_jobs.data();
      const auto r=Handoff(q,slot,owner,mode);
      REQUIRE(!r.mismatch); checked+=r.checked; swapped+=r.swapped;
      if (r.swapped) {
        REQUIRE(q.Acquire()==identity); REQUIRE(identity->pre_jobs.data()==closures);
      } else { slot->Reset(); slot->TakeFrom(*owner); owner->Reset(); }
      q.SubmitBatch(true,threshold);
    }
    expected_jobs+=JobValue(seq);
  }
  q.Sync(); REQUIRE(consumed==n); REQUIRE(mismatches==0); REQUIRE(jobs==expected_jobs);
  REQUIRE(mode!=2 || checked==swapped); REQUIRE(mode==0 || swapped>0);
  REQUIRE(q.stats().full_waits>0);
  std::cout<<"PASS mode="<<mode<<" batch="<<threshold<<" entries="<<consumed<<" swaps="<<swapped
           <<" checks="<<checked<<" mismatches="<<mismatches<<" full_waits="<<q.stats().full_waits<<" jobs_exactly_once=1\n";
}
static void HeldDrawJobsTest() {
  std::vector<uint32_t> order;
  uint64_t jobs=0;
  nr::RecordQueue<Draw,128> q([&](Draw& d) {
    order.push_back(d.ordinal);
    for (auto& job:d.pre_jobs) job();
    d.pre_jobs.clear();
  });
  auto owner=std::make_unique<Draw>();
  Fill(*owner,200,&jobs); Draw saved=*owner;
  for (uint32_t i=1;i<=20;++i) {
    Draw* slot=q.Acquire(); Fill(*slot,i,&jobs,true); q.SubmitBatch(false,64);
  }
  q.Sync(); REQUIRE(SameOutput(*owner,saved)); REQUIRE(order.size()==20);
  Draw wrong; Draw* slot=q.Acquire(); Draw* identity=owner.get();
  const auto rejected=Handoff(q,&wrong,owner,2);
  REQUIRE(rejected.mismatch && !rejected.swapped); REQUIRE(owner.get()==identity);
  REQUIRE(SameOutput(*owner,saved)); REQUIRE(q.Acquire()==slot);
  const auto accepted=Handoff(q,slot,owner,2);
  REQUIRE(accepted.swapped && accepted.checked && !accepted.mismatch);
  q.SubmitBatch(true,64); q.Sync();
  REQUIRE(order.size()==21 && order.back()==200);
  uint64_t sum=JobValue(200);
  for (uint32_t i=1;i<=20;++i) { REQUIRE(order[i-1]==i); sum+=JobValue(i); }
  REQUIRE(jobs==sum);
  std::cout<<"PASS jobs-only publication + synchronous drain with private draw held; rejected exchange leaves legacy input intact\n";
}
int main() {
  ComparatorTest();
  HeldDrawJobsTest();
  for (int mode:{0,1,2}) for (uint32_t threshold:{1u,64u}) Stress(mode,threshold);
  std::cout<<"PASS draw-swap host suite (72000 entries)\n";
}
