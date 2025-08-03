#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <limits>

namespace perf
{
    class PerfBuffer
    {
        public:
            PerfBuffer(size_t size) {
                this->mSize = size;
                mSamples.resize(size, 0);
                updateMinMax();
            }
            ~PerfBuffer() {
                mSamples.clear();
            }

            void addSample(uint32_t sample) {
                currentIndex = (currentIndex + 1) % mSize;
                mSamples[currentIndex] = sample;
                updateMinMax();
            }

            void clear() {
                for (auto& sample : mSamples) {
                    sample = 0;
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
                if (mSamples.empty()) {
                    minVal = 0;
                    maxVal = 0;
                    return;
                }
                minVal = *std::min_element(mSamples.begin(), mSamples.end());
                maxVal = *std::max_element(mSamples.begin(), mSamples.end());
            }

            size_t mSize;
            std::vector<uint32_t> mSamples;
            size_t currentIndex = 0;
            uint32_t minVal = 0;
            uint32_t maxVal = 0;
    };
}