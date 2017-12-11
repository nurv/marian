#include <boost/timer/timer.hpp>
#include "common/search.h"
#include "common/sentences.h"
#include "common/god.h"
#include "common/history.h"
#include "common/histories.h"
#include "common/filter.h"
#include "common/base_matrix.h"

#ifdef CUDA
#include <cuda.h>
#include <cuda_runtime.h>
#endif

using namespace std;

namespace amunmt {

Search::Search(const God &god)
  : deviceInfo_(god.GetNextDevice()),
    scorers_(god.GetScorers(deviceInfo_)),
    filter_(god.GetFilter()),
    maxBeamSize_(god.Get<size_t>("beam-size")),
    normalizeScore_(god.Get<bool>("normalize")),
    bestHyps_(god.GetBestHyps(deviceInfo_))
{}


Search::~Search() {
#ifdef CUDA
  if (deviceInfo_.deviceType == GPUDevice) {
    cudaSetDevice(deviceInfo_.deviceId);
  }
#endif
}

void Search::CleanAfterTranslation()
{
  for (auto scorer : scorers_) {
    scorer->CleanUpAfterSentence();
  }
}

std::shared_ptr<Histories> Search::Translate(const Sentences& sentences)
{
  /*
  assert(scorers_.size() == 1);
  std::shared_ptr<Histories> histories = scorers_[0]->Translate(*bestHyps_, sentences);
  return histories;
  */
  boost::timer::cpu_timer timer;

  if (filter_) {
    FilterTargetVocab(sentences);
  }

  States states = Encode(sentences);
  States nextStates = NewStates();
  std::vector<uint> beamSizes(sentences.size(), 1);

  std::shared_ptr<Histories> histories(new Histories(sentences, normalizeScore_));
  Beam prevHyps = histories->GetFirstHyps();

  for (size_t decoderStep = 0; decoderStep < 3 * sentences.GetMaxLength(); ++decoderStep) {
    for (size_t i = 0; i < scorers_.size(); i++) {
      scorers_[i]->Decode(*states[i], *nextStates[i], beamSizes);
    }

    if (decoderStep == 0) {
      for (auto& beamSize : beamSizes) {
        beamSize = maxBeamSize_;
      }
    }
    //cerr << "beamSizes=" << Debug(beamSizes, 1) << endl;

    bool hasSurvivors = CalcBeam(histories, beamSizes, prevHyps, *states[0], *nextStates[0]);
    //bool hasSurvivors = scorers_[0]->CalcBeam(*bestHyps_, histories, beamSizes, prevHyps, *states[0], *nextStates[0], filterIndices_);
    if (!hasSurvivors) {
      break;
    }
  }

  CleanAfterTranslation();

  LOG(progress)->info("Search took {}", timer.format(3, "%ws"));
  return histories;

}


States Search::Encode(const Sentences& sentences) {
  States states;
  for (auto& scorer : scorers_) {
    scorer->Encode(sentences);
    auto state = scorer->NewState();
    scorer->BeginSentenceState(*state, sentences.size());
    states.emplace_back(state);
  }
  return states;
}

bool Search::CalcBeam(
    std::shared_ptr<Histories>& histories,
    std::vector<uint>& beamSizes,
    Beam& prevHyps,
    State& state,
    State& nextState)
{
    size_t batchSize = beamSizes.size();
    Beams beams(batchSize);
    bestHyps_->CalcBeam(prevHyps, scorers_[0], filterIndices_, beams, beamSizes);
    histories->Add(beams);

    Beam survivors;
    for (size_t batchId = 0; batchId < batchSize; ++batchId) {
      for (auto& h : beams[batchId]) {
        if (h->GetWord() != EOS_ID) {
          survivors.push_back(h);
        } else {
          --beamSizes[batchId];
        }
      }
    }

    if (survivors.size() == 0) {
      return false;
    }

    scorers_[0]->AssembleBeamState(nextState, survivors, state);

    //cerr << "survivors=" << survivors.size() << endl;
    prevHyps.swap(survivors);
    return true;
}


States Search::NewStates() const {
  States states;
  for (auto& scorer : scorers_) {
    states.emplace_back(scorer->NewState());
  }
  return states;
}

void Search::FilterTargetVocab(const Sentences& sentences) {
  size_t vocabSize = scorers_[0]->GetVocabSize();
  std::set<Word> srcWords;
  for (size_t i = 0; i < sentences.size(); ++i) {
    const Sentence& sentence = sentences.Get(i);
    for (const auto& srcWord : sentence.GetWords()) {
      srcWords.insert(srcWord);
    }
  }

  filterIndices_ = filter_->GetFilteredVocab(srcWords, vocabSize);
  for (auto& scorer : scorers_) {
    scorer->Filter(filterIndices_);
  }
}



}

