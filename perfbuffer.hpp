#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace perf
{
    class PerfBuffer
    {
        public:
            PerfBuffer(size_t size) {
                this->mSize = size;
                mSamples.resize(size, 0);
            }
            ~PerfBuffer() {
                mSamples.clear();
            }

            void addSample(uint32_t sample) {
                currentIndex = (currentIndex + 1) % mSize;
                mSamples[currentIndex] = sample;
            }

            void clear() {
                for (auto& sample : mSamples) {
                    sample = 0;
                }
                currentIndex = 0;
            }

            std::vector<uint32_t> getSamples() const {
                std::vector<uint32_t> samples;
                for (size_t i = 0; i < mSize; ++i) {
                    samples.push_back(mSamples[(currentIndex + i) % mSize]);
                }
                return samples;
            }

        private:
            size_t mSize;
            std::vector<uint32_t> mSamples;
            size_t currentIndex = 0;
    };
}