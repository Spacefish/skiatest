#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <limits>
#include <set>

namespace perf
{
    class PerfBuffer
    {
        public:
            PerfBuffer(size_t size) : mSize(size), mSamples(size, 0), currentIndex(0), minVal(0), maxVal(0) {
                for (size_t i = 0; i < mSize; ++i) {
                    mValueSet.insert(0);
                }
                updateMinMax();
            }
            ~PerfBuffer() {
                mSamples.clear();
                mValueSet.clear();
            }

            void addSample(uint32_t sample) {
                currentIndex = (currentIndex + 1) % mSize;
                uint32_t old = mSamples[currentIndex];
                auto it = mValueSet.find(old);
                if (it != mValueSet.end()) {
                    mValueSet.erase(it);
                }
                mSamples[currentIndex] = sample;
                mValueSet.insert(sample);
                updateMinMax();
            }

            void clear() {
                for (auto& sample : mSamples) {
                    sample = 0;
                }
                mValueSet.clear();
                for (size_t i = 0; i < mSize; ++i) {
                    mValueSet.insert(0);
                }
                currentIndex = 0;
                minVal = 0;
                maxVal = 0;
            }

            uint32_t getOrderedSample(size_t i) const {
                // i = 0: oldest sample, i = mSize-1: newest sample
                size_t idx = (currentIndex + 1 + i) % mSize;
                return mSamples[idx];
            }

            uint32_t getMin() const { return minVal; }
            uint32_t getMax() const { return maxVal; }

        private:
            void updateMinMax() {
                if (mValueSet.empty()) {
                    minVal = 0;
                    maxVal = 0;
                    return;
                }
                minVal = *mValueSet.begin();
                maxVal = *mValueSet.rbegin();
            }

            size_t mSize;
            std::vector<uint32_t> mSamples;
            std::multiset<uint32_t> mValueSet;
            size_t currentIndex = 0;
            uint32_t minVal = 0;
            uint32_t maxVal = 0;
    };
}