/**********************************************************************
 
 Audacity: A Digital Audio Editor
 
 PlaybackSchedule.cpp
 
 Paul Licameli split from AudioIOBase.cpp
 
 **********************************************************************/

#include "PlaybackSchedule.h"

#include "AudioIOBase.h"
#include "Envelope.h"
#include "Mix.h"
#include "SampleCount.h"

#include <cmath>

PlaybackPolicy::~PlaybackPolicy() = default;

void PlaybackPolicy::Initialize( PlaybackSchedule &, double rate )
{
   mRate = rate;
}

void PlaybackPolicy::Finalize( PlaybackSchedule & ){}

Mixer::WarpOptions PlaybackPolicy::MixerWarpOptions(PlaybackSchedule &schedule)
{
   return Mixer::WarpOptions{ schedule.mEnvelope };
}

PlaybackPolicy::BufferTimes
PlaybackPolicy::SuggestedBufferTimes(PlaybackSchedule &)
{
   return { 4.0, 4.0, 10.0 };
}

bool PlaybackPolicy::AllowSeek(PlaybackSchedule &)
{
   return true;
}

bool PlaybackPolicy::Done( PlaybackSchedule &schedule,
   unsigned long outputFrames)
{
   // Called from portAudio thread, use GetTrackTime()
   auto diff = schedule.GetTrackTime() - schedule.mT1;
   if (schedule.ReversedTime())
      diff *= -1;
   return sampleCount(floor(diff * mRate + 0.5)) >= 0 &&
      // Require also that output frames are all consumed from ring buffer
      outputFrames == 0;
}

double PlaybackPolicy::OffsetTrackTime(
   PlaybackSchedule &schedule, double offset )
{
   const auto time = schedule.GetTrackTime() + offset;
   schedule.RealTimeInit( time );
   return time;
}

std::chrono::milliseconds PlaybackPolicy::SleepInterval(PlaybackSchedule &)
{
   using namespace std::chrono;
   return 10ms;
}

PlaybackSlice
PlaybackPolicy::GetPlaybackSlice(PlaybackSchedule &schedule, size_t available)
{
   // How many samples to produce for each channel.
   const auto realTimeRemaining = schedule.RealTimeRemaining();
   auto frames = available;
   auto toProduce = frames;
   double deltat = frames / mRate;

   if (deltat > realTimeRemaining)
   {
      // Produce some extra silence so that the time queue consumer can
      // satisfy its end condition
      const double extraRealTime = (TimeQueueGrainSize + 1) / mRate;
      auto extra = std::min( extraRealTime, deltat - realTimeRemaining );
      auto realTime = realTimeRemaining + extra;
      frames = realTime * mRate;
      toProduce = realTimeRemaining * mRate;
      schedule.RealTimeAdvance( realTime );
   }
   else
      schedule.RealTimeAdvance( deltat );

   return { available, frames, toProduce };
}

std::pair<double, double>
PlaybackPolicy::AdvancedTrackTime( PlaybackSchedule &schedule,
   double trackTime, size_t nSamples )
{
   auto realDuration = nSamples / mRate;
   if (schedule.ReversedTime())
      realDuration *= -1.0;

   if (schedule.mEnvelope)
      trackTime =
         schedule.SolveWarpedLength(trackTime, realDuration);
   else
      trackTime += realDuration;
   
   if ( trackTime >= schedule.mT1 )
      return { schedule.mT1, std::numeric_limits<double>::infinity() };
   else
      return { trackTime, trackTime };
}

bool PlaybackPolicy::RepositionPlayback(
   PlaybackSchedule &, const Mixers &, size_t, size_t)
{
   return true;
}

bool PlaybackPolicy::Looping(const PlaybackSchedule &) const
{
   return false;
}

namespace {
//! The old default playback policy plays once and consumes no messages
struct OldDefaultPlaybackPolicy final : PlaybackPolicy {
   ~OldDefaultPlaybackPolicy() override = default;
};
}

PlaybackPolicy &PlaybackSchedule::GetPolicy()
{
   if (mPolicyValid.load(std::memory_order_acquire) && mpPlaybackPolicy)
      return *mpPlaybackPolicy;

   static OldDefaultPlaybackPolicy defaultPolicy;
   return defaultPolicy;
}

const PlaybackPolicy &PlaybackSchedule::GetPolicy() const
{
   return const_cast<PlaybackSchedule&>(*this).GetPolicy();
}

NewDefaultPlaybackPolicy::NewDefaultPlaybackPolicy(
   double trackEndTime, double loopEndTime, bool loopEnabled )
   : mTrackEndTime{ trackEndTime }
   , mLoopEndTime{ loopEndTime }
   , mLoopEnabled{ loopEnabled }
{}

NewDefaultPlaybackPolicy::~NewDefaultPlaybackPolicy() = default;

void NewDefaultPlaybackPolicy::Initialize(
   PlaybackSchedule &schedule, double rate )
{
   PlaybackPolicy::Initialize(schedule, rate);
   schedule.mMessageChannel.Write( {
      schedule.mT0, mLoopEndTime, mLoopEnabled } );
}

PlaybackPolicy::BufferTimes
NewDefaultPlaybackPolicy::SuggestedBufferTimes(PlaybackSchedule &)
{
   // Shorter times than in the default policy so that responses to changes of
   // selection don't lag too much
   return { 0.5, 0.5, 1.0 };
}

bool NewDefaultPlaybackPolicy::RevertToOldDefault(const PlaybackSchedule &schedule) const
{
   return !mLoopEnabled ||
      // Even if loop is enabled, ignore it if right of looping region
      schedule.mTimeQueue.GetLastTime() > mLoopEndTime;
}

bool NewDefaultPlaybackPolicy::Done(
   PlaybackSchedule &schedule, unsigned long outputFrames )
{
   if (RevertToOldDefault(schedule))
      return PlaybackPolicy::Done(schedule, outputFrames);
   return false;
}

PlaybackSlice
NewDefaultPlaybackPolicy::GetPlaybackSlice(
   PlaybackSchedule &schedule, size_t available)
{
   if (RevertToOldDefault(schedule))
      return PlaybackPolicy::GetPlaybackSlice(schedule, available);

   // How many samples to produce for each channel.
   const auto realTimeRemaining = std::max(0.0, schedule.RealTimeRemaining());
   auto frames = available;
   auto toProduce = frames;
   double deltat = frames / mRate;

   mRemaining = realTimeRemaining * mRate;
   if (deltat > realTimeRemaining)
   {
      toProduce = frames = mRemaining;
      schedule.RealTimeAdvance( realTimeRemaining );
   }
   else
      schedule.RealTimeAdvance( deltat );

   // Don't fall into an infinite loop, if loop-playing a selection
   // that is so short, it has no samples: detect that case
   if (frames == 0) {
      bool progress = (schedule.mWarpedTime != 0.0);
      if (!progress)
         // Cause FillPlayBuffers to make progress, filling all available with 0
         frames = available, toProduce = 0;
   }
   return { available, frames, toProduce };
}

std::pair<double, double> NewDefaultPlaybackPolicy::AdvancedTrackTime(
   PlaybackSchedule &schedule, double trackTime, size_t nSamples )
{
   if (RevertToOldDefault(schedule))
      return PlaybackPolicy::AdvancedTrackTime(schedule, trackTime, nSamples);

   mRemaining -= std::min(mRemaining, nSamples);
   if ( mRemaining == 0 )
      // Wrap to start
      return { schedule.mT1, schedule.mT0 };

   // Defense against cases that might cause loops not to terminate
   if ( fabs(schedule.mT0 - schedule.mT1) < 1e-9 )
      return {schedule.mT0, schedule.mT0};

   auto realDuration = nSamples / mRate;
   if (schedule.ReversedTime())
      realDuration *= -1.0;

   if (schedule.mEnvelope)
      trackTime =
         schedule.SolveWarpedLength(trackTime, realDuration);
   else
      trackTime += realDuration;

   return { trackTime, trackTime };
}

bool NewDefaultPlaybackPolicy::RepositionPlayback(
   PlaybackSchedule &schedule, const Mixers &playbackMixers,
   size_t frames, size_t available )
{
   // This executes in the TrackBufferExchange thread
   auto data = schedule.mMessageChannel.Read();

   bool empty = (data.mT0 >= data.mT1);
   bool kicked = false;

   // Amount in seconds by which right boundary can be moved left of the play
   // head, yet loop play in progress will still capture the head
   constexpr auto allowance = 0.5;

   bool loopWasEnabled = !RevertToOldDefault(schedule);
   mLoopEnabled = data.mLoopEnabled && !empty &&
      schedule.mTimeQueue.GetLastTime() <= data.mT1 + allowance;

   auto mine = std::tie(schedule.mT0, mLoopEndTime);
   auto theirs = std::tie(data.mT0, data.mT1);
   if ( mLoopEnabled ? (mine != theirs) : loopWasEnabled ) {
      kicked = true;
      if (!empty) {
         mine = theirs;
         schedule.mT1 = data.mT1;
      }
      if (!mLoopEnabled)
         // Continue play to the end
         schedule.mT1 = std::max(schedule.mT0, mTrackEndTime);
      schedule.mWarpedLength = schedule.RealDuration(schedule.mT1);

      auto newTime = schedule.mTimeQueue.GetLastTime();
#if 0
      // This would make play jump forward or backward into the adjusted
      // looping region if not already in it
      newTime = std::clamp(newTime, schedule.mT0, schedule.mT1);
#endif

      if (newTime >= schedule.mT1 && mLoopEnabled)
         newTime = schedule.mT0;

      // So that the play head will redraw in the right place:
      schedule.mTimeQueue.SetLastTime(newTime);

      schedule.RealTimeInit(newTime);
      const auto realTimeRemaining = std::max(0.0, schedule.RealTimeRemaining());
      mRemaining = realTimeRemaining * mRate;
   }

   if (RevertToOldDefault(schedule) && !kicked)
      return PlaybackPolicy::RepositionPlayback( schedule, playbackMixers,
         frames, available);

   // msmeyer: If playing looped, check if we are at the end of the buffer
   // and if yes, restart from the beginning.
   if (mRemaining <= 0)
   {
      for (auto &pMixer : playbackMixers)
         pMixer->SetTimesAndSpeed( schedule.mT0, schedule.mT1, 1.0, true );
      schedule.RealTimeRestart();
   }
   else if (kicked)
   {
      const auto time = schedule.mTimeQueue.GetLastTime();
      for (auto &pMixer : playbackMixers) {
         // So that the mixer will fetch the next samples from the right place:
         pMixer->SetTimesAndSpeed( schedule.mT0, schedule.mT1, 1.0 );
         pMixer->Reposition(time, true);
      }
   }
   return false;
}

bool NewDefaultPlaybackPolicy::Looping( const PlaybackSchedule & ) const
{
   return mLoopEnabled;
}

void PlaybackSchedule::Init(
   const double t0, const double t1,
   const AudioIOStartStreamOptions &options,
   const RecordingSchedule *pRecordingSchedule )
{
   mpPlaybackPolicy.reset();

   if ( pRecordingSchedule )
      // It does not make sense to apply the time warp during overdub recording,
      // which defeats the purpose of making the recording synchronized with
      // the existing audio.  (Unless we figured out the inverse warp of the
      // captured samples in real time.)
      // So just quietly ignore the time track.
      mEnvelope = nullptr;
   else
      mEnvelope = options.envelope;

   mT0      = t0;
   if (pRecordingSchedule)
      mT0 -= pRecordingSchedule->mPreRoll;

   mT1      = t1;
   if (pRecordingSchedule)
      // adjust mT1 so that we don't give paComplete too soon to fill up the
      // desired length of recording
      mT1 -= pRecordingSchedule->mLatencyCorrection;

   // Main thread's initialization of mTime
   SetTrackTime( mT0 );

   if (options.policyFactory)
      mpPlaybackPolicy = options.policyFactory();

   mWarpedTime = 0.0;
   mWarpedLength = RealDuration(mT1);

   mPolicyValid.store(true, std::memory_order_release);

   mMessageChannel.Initialize();
}

double PlaybackSchedule::ComputeWarpedLength(double t0, double t1) const
{
   if (mEnvelope)
      return mEnvelope->IntegralOfInverse(t0, t1);
   else
      return t1 - t0;
}

double PlaybackSchedule::SolveWarpedLength(double t0, double length) const
{
   if (mEnvelope)
      return mEnvelope->SolveIntegralOfInverse(t0, length);
   else
      return t0 + length;
}

double PlaybackSchedule::RealDuration(double trackTime1) const
{
   return fabs(RealDurationSigned(trackTime1));
}

double PlaybackSchedule::RealDurationSigned(double trackTime1) const
{
   return ComputeWarpedLength(mT0, trackTime1);
}

double PlaybackSchedule::RealTimeRemaining() const
{
   return mWarpedLength - mWarpedTime;
}

void PlaybackSchedule::RealTimeAdvance( double increment )
{
   mWarpedTime += increment;
}

void PlaybackSchedule::RealTimeInit( double trackTime )
{
   mWarpedTime = RealDurationSigned( trackTime );
}

void PlaybackSchedule::RealTimeRestart()
{
   mWarpedTime = 0;
}

double RecordingSchedule::ToConsume() const
{
   return mDuration - Consumed();
}

double RecordingSchedule::Consumed() const
{
   return std::max( 0.0, mPosition + TotalCorrection() );
}

double RecordingSchedule::ToDiscard() const
{
   return std::max(0.0, -( mPosition + TotalCorrection() ) );
}

void PlaybackSchedule::TimeQueue::Clear()
{
   mData = Records{};
   mHead = {};
   mTail = {};
}

void PlaybackSchedule::TimeQueue::Resize(size_t size)
{
   mData.resize(size);
}

void PlaybackSchedule::TimeQueue::Producer(
   PlaybackSchedule &schedule, PlaybackSlice slice )
{
   auto &policy = schedule.GetPolicy();

   if ( mData.empty() )
      // Recording only.  Don't fill the queue.
      return;

   // Don't check available space:  assume it is enough because of coordination
   // with RingBuffer.
   auto index = mTail.mIndex;
   auto time = mLastTime;
   auto remainder = mTail.mRemainder;
   auto space = TimeQueueGrainSize - remainder;
   const auto size = mData.size();

   // Produce advancing times
   auto frames = slice.toProduce;
   while ( frames >= space ) {
      auto times = policy.AdvancedTrackTime( schedule, time, space );
      time = times.second;
      if (!std::isfinite(time))
         time = times.first;
      index = (index + 1) % size;
      mData[ index ].timeValue = time;
      frames -= space;
      remainder = 0;
      space = TimeQueueGrainSize;
   }
   // Last odd lot
   if ( frames > 0 ) {
      auto times = policy.AdvancedTrackTime( schedule, time, frames );
      time = times.second;
      if (!std::isfinite(time))
         time = times.first;
      remainder += frames;
      space -= frames;
   }

   // Produce constant times if there is also some silence in the slice
   frames = slice.frames - slice.toProduce;
   while ( frames > 0 && frames >= space ) {
      index = (index + 1) % size;
      mData[ index ].timeValue = time;
      frames -= space;
      remainder = 0;
      space = TimeQueueGrainSize;
   }

   mLastTime = time;
   mTail.mRemainder = remainder + frames;
   mTail.mIndex = index;
}

double PlaybackSchedule::TimeQueue::GetLastTime() const
{
   return mLastTime;
}

void PlaybackSchedule::TimeQueue::SetLastTime(double time)
{
   mLastTime = time;
}

double PlaybackSchedule::TimeQueue::Consumer( size_t nSamples, double rate )
{
   if ( mData.empty() ) {
      // Recording only.  No scrub or playback time warp.  Don't use the queue.
      return ( mLastTime += nSamples / rate );
   }

   // Don't check available space:  assume it is enough because of coordination
   // with RingBuffer.
   auto remainder = mHead.mRemainder;
   auto space = TimeQueueGrainSize - remainder;
   const auto size = mData.size();
   if ( nSamples >= space ) {
      remainder = 0,
      mHead.mIndex = (mHead.mIndex + 1) % size,
      nSamples -= space;
      if ( nSamples >= TimeQueueGrainSize )
         mHead.mIndex =
            (mHead.mIndex + ( nSamples / TimeQueueGrainSize ) ) % size,
         nSamples %= TimeQueueGrainSize;
   }
   mHead.mRemainder = remainder + nSamples;
   return mData[ mHead.mIndex ].timeValue;
}

void PlaybackSchedule::TimeQueue::Prime(double time)
{
   mHead = mTail = {};
   mLastTime = time;
   if ( !mData.empty() )
      mData[0].timeValue = time;
}

#include "ViewInfo.h"
void PlaybackSchedule::MessageProducer( PlayRegionEvent &evt)
{
   // This executes in the main thread
   auto *pRegion = evt.pRegion.get();
   if ( !pRegion )
      return;
   const auto &region = *pRegion;

   mMessageChannel.Write( {
      region.GetStart(), region.GetEnd(), region.Active()
   } );
}
