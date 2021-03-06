/**
 * Model the bias in read start positions across a set of transcripts.
 * This class is similar to and inspired by the FragmentLengthDistribution
 * class, which was itself modified from lengthdistribution.cpp ---
 * originally written by Adam Roberts as part of the eXpress software.
 * Rob Patro; 2014, 2015
 */


#include "FragmentStartPositionDistribution.hpp"
#include "SalmonMath.hpp"
#include <numeric>
#include <cassert>
#include <boost/assign.hpp>
#include <iostream>
#include <fstream>
#include <boost/math/distributions/binomial.hpp>
#include <boost/math/distributions/normal.hpp>

using namespace std;

FragmentStartPositionDistribution::FragmentStartPositionDistribution(uint32_t numBins)
    : numBins_(numBins),
      pmf_(numBins+2),
      cmf_(numBins+2),
      totMass_(salmon::math::LOG_1),
      isUpdated_(false) {

  using salmon::math::logAdd;
  double uniMass = log(1.0 / numBins_);
  for (auto i = 1; i <= numBins_; ++i) {
    pmf_[i] = uniMass;
    cmf_[i] = log(static_cast<double>(i)) + uniMass;
  }
}

inline void logAddMass(tbb::atomic<double>& bin,
                       double newMass) {
      double oldVal = bin;
      double retVal = oldVal;
      double newVal = 0.0;
      do {
          oldVal = retVal;
          newVal = salmon::math::logAdd(oldVal, newMass);
          retVal = bin.compare_and_swap(newVal, oldVal);
      } while (retVal != oldVal);
}

void FragmentStartPositionDistribution::addVal(
        int32_t hitPos,
        uint32_t txpLen,
        double mass) {

    using salmon::math::logAdd;
    if (hitPos >= txpLen) return; // hit should happen within the transcript

    if (hitPos < 0) { hitPos = 0; }
    double logLen = log(txpLen);

    // Modified from: https://github.com/deweylab/RSEM/blob/master/RSPD.h
    int i;
    // Fraction along the transcript where this hit occurs
    double a = hitPos * 1.0 / txpLen;
    double b;

    for (i = ((long long)hitPos) * numBins_ / txpLen + 1;
         i < (((long long)hitPos + 1) * numBins_ - 1) / txpLen + 1; i++) {
        b = i * 1.0 / numBins_;
        double updateMass = log(b - a) + logLen + mass;
        logAddMass(pmf_[i], updateMass);
        logAddMass(totMass_, updateMass);
        a = b;
    }
    b = (hitPos + 1.0) / txpLen;
    double updateMass = log(b - a) + logLen + mass;
    logAddMass(pmf_[i], updateMass);
    logAddMass(totMass_, updateMass);
}

double FragmentStartPositionDistribution::evalCDF(int32_t hitPos, uint32_t txpLen) {
    int i = (static_cast<long long>(hitPos) * numBins_) / txpLen;
    double val = hitPos * 1.0 / txpLen * numBins_;
    return salmon::math::logAdd(cmf_[i], std::log(val - i) + pmf_[i+1]);
}

void FragmentStartPositionDistribution::update() {
    std::lock_guard<std::mutex> lg(fspdMut_);
    if (!isUpdated_) {
        for (int i = 1; i <= numBins_; i++) {
            pmf_[i] = pmf_[i] - totMass_;
            cmf_[i] = salmon::math::logAdd(cmf_[i - 1], pmf_[i]);
        }
        isUpdated_ = true;
    }
}

double FragmentStartPositionDistribution::operator()(
        int32_t hitPos,
        uint32_t txpLen,
        double logEffLen) {
    
    if (hitPos < 0) { hitPos = 0; }
    assert(hitPos < txpLen);
    if (hitPos >= txpLen) {
	    std::cerr << "\n\nhitPos = " << hitPos << ", txpLen = " << txpLen << "!!\n\n\n";
	    return salmon::math::LOG_0;
    }
    // If we haven't updated the CDF yet, then
    // just return log(1 / effLen);
    if (!isUpdated_) {
        return -logEffLen;
    }

    double a = hitPos * (1.0 / txpLen);

    double effLen = std::exp(logEffLen);
    if (effLen >= txpLen) {
	    effLen = txpLen - 1;
    }
    double denom = evalCDF(static_cast<int32_t>(effLen), txpLen);
    return ((denom >= salmon::math::LOG_EPSILON) ?
            salmon::math::logSub(evalCDF(hitPos + 1, txpLen), evalCDF(hitPos, txpLen)) -
	    denom : 0.0); 

    //return salmon::math::logSub(evalCDF(hitPos + 1, txpLen), evalCDF(hitPos, txpLen));
}

double FragmentStartPositionDistribution::totMass() const {
  return totMass_;
}

std::string FragmentStartPositionDistribution::toString() const {
    std::stringstream ss;
    for (size_t i = 0; i < pmf_.size(); ++i) {
        ss << std::exp(pmf_[i] - totMass_);
        if (i != pmf_.size() - 1) { ss << '\t'; }
    }
    ss << "\n";
    return ss.str();
}

