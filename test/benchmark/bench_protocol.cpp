#include <benchmark/benchmark.h>
#include "protocol/converter.h"

using namespace gateway::protocol;

static const std::string kOpenAI1KB = R"({
  "model": "gpt-4o",
  "stream": true,
  "messages": [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Explain quantum computing in simple terms."},
    {"role": "assistant", "content": "Quantum computing uses qubits that can be 0 and 1 simultaneously."},
    {"role": "user", "content": "How is that different from classical bits?"},
    {"role": "assistant", "content": "Classical bits are either 0 or 1, never both."},
    {"role": "user", "content": "Give me a practical example of quantum advantage."},
    {"role": "assistant", "content": "Drug discovery can simulate molecular interactions exponentially faster."},
    {"role": "user", "content": "What about cryptography?"},
    {"role": "assistant", "content": "Shor's algorithm can factor large numbers, breaking RSA encryption."},
    {"role": "user", "content": "Summarize the key points so far."}
  ],
  "max_tokens": 1024,
  "temperature": 0.7
})";

static const std::string kOpenAI10KB = [] {
    std::string base = R"({"model":"gpt-4o","stream":true,"max_tokens":4096,"messages":[)";
    for (int i = 0; i < 50; ++i) {
        if (i > 0) base += ",";
        base += R"({"role":"user","content":"This is message number )" + std::to_string(i) +
                R"( with enough padding text to reach approximately 200 bytes per message entry in the JSON.)";
        base += R"( The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs."})";
    }
    base += "]}";
    return base;
}();

static const std::string kAnthropicBody = R"({
  "model": "claude-sonnet-4-20250514",
  "max_tokens": 2048,
  "stream": true,
  "system": [{"type": "text", "text": "You are an expert code reviewer."}],
  "messages": [
    {"role": "user", "content": [{"type": "text", "text": "Review this function for bugs."}]},
    {"role": "assistant", "content": [{"type": "text", "text": "I see a potential null dereference on line 42."}]},
    {"role": "user", "content": [{"type": "text", "text": "How should I fix it?"}]}
  ]
})";

static void BM_ParseOpenAI_1KB(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Converter::parse(kOpenAI1KB, Format::OpenAIChatCompletions);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * kOpenAI1KB.size());
}
BENCHMARK(BM_ParseOpenAI_1KB);

static void BM_ParseOpenAI_10KB(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Converter::parse(kOpenAI10KB, Format::OpenAIChatCompletions);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * kOpenAI10KB.size());
}
BENCHMARK(BM_ParseOpenAI_10KB);

static void BM_ParseAnthropic(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Converter::parse(kAnthropicBody, Format::Anthropic);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * kAnthropicBody.size());
}
BENCHMARK(BM_ParseAnthropic);

static void BM_ConvertOpenAIToAnthropic(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Converter::convert_request(
            kOpenAI1KB, Format::OpenAIChatCompletions, Format::Anthropic, "claude-sonnet-4-20250514");
        benchmark::DoNotOptimize(result.body);
    }
    state.SetBytesProcessed(state.iterations() * kOpenAI1KB.size());
}
BENCHMARK(BM_ConvertOpenAIToAnthropic);

static void BM_ConvertAnthropicToOpenAI(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Converter::convert_request(
            kAnthropicBody, Format::Anthropic, Format::OpenAIChatCompletions, "gpt-4o");
        benchmark::DoNotOptimize(result.body);
    }
    state.SetBytesProcessed(state.iterations() * kAnthropicBody.size());
}
BENCHMARK(BM_ConvertAnthropicToOpenAI);

static void BM_ConvertOpenAIToGemini(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Converter::convert_request(
            kOpenAI1KB, Format::OpenAIChatCompletions, Format::Gemini, "gemini-2.0-flash");
        benchmark::DoNotOptimize(result.body);
    }
    state.SetBytesProcessed(state.iterations() * kOpenAI1KB.size());
}
BENCHMARK(BM_ConvertOpenAIToGemini);

static const std::string kSSEOpenAI =
    R"({"id":"chatcmpl-abc","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]})";

static void BM_StreamEventTransform(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Converter::convert_stream_event(
            kSSEOpenAI, Format::OpenAIChatCompletions, Format::Anthropic);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * kSSEOpenAI.size());
}
BENCHMARK(BM_StreamEventTransform);
