// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/renderer/api/atom_api_spell_check_client.h"

#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/spellcheck/renderer/spellcheck_worditerator.h"
#include "native_mate/converter.h"
#include "native_mate/dictionary.h"
#include "native_mate/function_template.h"
#include "shell/common/native_mate_converters/string16_converter.h"
#include "third_party/blink/public/web/web_text_checking_completion.h"
#include "third_party/blink/public/web/web_text_checking_result.h"
#include "third_party/icu/source/common/unicode/uscript.h"

namespace electron {

namespace api {

namespace {

bool HasWordCharacters(const base::string16& text, int index) {
  const base::char16* data = text.data();
  int length = text.length();
  while (index < length) {
    uint32_t code = 0;
    U16_NEXT(data, index, length, code);
    UErrorCode error = U_ZERO_ERROR;
    if (uscript_getScript(code, &error) != USCRIPT_COMMON)
      return true;
  }
  return false;
}

}  // namespace

using Word = atom::SpellcheckLanguage::Word;

class SpellCheckClient::SpellcheckRequest {
 public:
  SpellcheckRequest(
      const base::string16& text,
      std::unique_ptr<blink::WebTextCheckingCompletion> completion)
      : text_(text), completion_(std::move(completion)) {}
  ~SpellcheckRequest() {}

  const base::string16& text() const { return text_; }
  blink::WebTextCheckingCompletion* completion() { return completion_.get(); }
  std::unordered_set<Word>& wordlist() { return word_list_; }
  std::vector<blink::WebTextCheckingResult>* misspelled_words() {
    return &results_;
  }
  void setCount(int count) { remaining_ = count; }
  void decrement() { remaining_--; }
  int remaining() { return remaining_; }

 private:
  base::string16 text_;                 // Text to be checked in this task.
  std::unordered_set<Word> word_list_;  // List of Words found in text
  // The interface to send the misspelled ranges to WebKit.
  std::unique_ptr<blink::WebTextCheckingCompletion> completion_;
  std::vector<blink::WebTextCheckingResult> results_;
  int remaining_ = 0;

  DISALLOW_COPY_AND_ASSIGN(SpellcheckRequest);
};

SpellCheckClient::SpellCheckClient(const std::vector<std::string>& languages,
                                   v8::Isolate* isolate,
                                   v8::Local<v8::Object> provider)
    : pending_request_param_(nullptr),
      isolate_(isolate),
      context_(isolate, isolate->GetCurrentContext()),
      provider_(isolate, provider) {
  DCHECK(!context_.IsEmpty());
  for (const auto& language : languages) {
    AddSpellcheckLanguage(language);
  }

  // Persistent the method.
  mate::Dictionary dict(isolate, provider);
  dict.Get("spellCheck", &spell_check_);
}

SpellCheckClient::~SpellCheckClient() {
  context_.Reset();
}

void SpellCheckClient::RequestCheckingOfText(
    const blink::WebString& textToCheck,
    std::unique_ptr<blink::WebTextCheckingCompletion> completionCallback) {
  base::string16 text(textToCheck.Utf16());
  // Ignore invalid requests.
  if (text.empty() || !HasWordCharacters(text, 0)) {
    completionCallback->DidCancelCheckingText();
    return;
  }

  // Clean up the previous request before starting a new request.
  if (pending_request_param_) {
    pending_request_param_->completion()->DidCancelCheckingText();
  }

  pending_request_param_.reset(
      new SpellcheckRequest(text, std::move(completionCallback)));

  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&SpellCheckClient::SpellCheckText, AsWeakPtr()));
}

bool SpellCheckClient::IsSpellCheckingEnabled() const {
  return true;
}

void SpellCheckClient::ShowSpellingUI(bool show) {}

bool SpellCheckClient::IsShowingSpellingUI() {
  return false;
}

void SpellCheckClient::UpdateSpellingUIWithMisspelledWord(
    const blink::WebString& word) {}

void SpellCheckClient::AddSpellcheckLanguage(const std::string& language) {
  languages_.push_back(std::make_unique<SpellcheckLanguage>());
  languages_.back()->Init(language);
}

void SpellCheckClient::SpellCheckText() {
  const auto& text = pending_request_param_->text();
  if (text.empty() || spell_check_.IsEmpty()) {
    pending_request_param_->completion()->DidCancelCheckingText();
    pending_request_param_ = nullptr;
    return;
  }
  SpellCheckScope scope(*this);
  auto& word_list = pending_request_param_->wordlist();
  pending_request_param_->setCount(languages_.size());
  std::unordered_map<std::string, std::set<base::string16>> lang_words;
  for (const auto& language : languages_) {
    auto words = language->SpellCheckText(text, word_list);
    lang_words.insert(std::make_pair(language->GetLanguageString(), words));
  }
  // Send out all the words data to the spellchecker to check
  SpellCheckWords(scope, lang_words);
}

void SpellCheckClient::OnSpellCheckDone(
    const std::vector<base::string16>& misspelled_words) {
  std::unordered_set<base::string16> misspelled(misspelled_words.begin(),
                                                misspelled_words.end());
  auto& word_list = pending_request_param_->wordlist();

  for (auto word = word_list.begin(); word != word_list.end(); ++word) {
    if (misspelled.find(word->text) != misspelled.end()) {
      // If this is a contraction, iterate through parts and accept the word
      // if none of them are misspelled
      if (!word->contraction_words.empty()) {
        auto all_correct = true;
        for (const auto& contraction_word : word->contraction_words) {
          if (misspelled.find(contraction_word) != misspelled.end()) {
            all_correct = false;
            break;
          }
        }
        if (all_correct)
          continue;
      }
      word->misspelled_count--;
    }
  }
  pending_request_param_->decrement();
  if (pending_request_param_->remaining() == 0) {
    std::vector<blink::WebTextCheckingResult> result;
    for (const auto& w : word_list) {
      if (w.misspelled_count == 0) {
        result.push_back(w.result);
      }
    }
    pending_request_param_->completion()->DidFinishCheckingText(result);
    pending_request_param_ = nullptr;
  }
}

void SpellCheckClient::SpellCheckWords(
    const SpellCheckScope& scope,
    const std::unordered_map<std::string, std::set<base::string16>>&
        lang_words) {
  DCHECK(!scope.spell_check_.IsEmpty());

  for (const auto& entry : lang_words) {
    const auto& language = entry.first;
    const auto& words = entry.second;
    v8::Local<v8::FunctionTemplate> templ = mate::CreateFunctionTemplate(
        isolate_,
        base::BindRepeating(&SpellCheckClient::OnSpellCheckDone, AsWeakPtr()));

    auto context = isolate_->GetCurrentContext();
    v8::Local<v8::Value> args[] = {
        mate::ConvertToV8(isolate_, language),
        mate::ConvertToV8(isolate_, words),
        templ->GetFunction(context).ToLocalChecked()};
    // Call javascript with the words and the callback function
    scope.spell_check_->Call(context, scope.provider_, 3, args).IsEmpty();
  }
}

SpellCheckClient::SpellCheckScope::SpellCheckScope(
    const SpellCheckClient& client)
    : handle_scope_(client.isolate_),
      context_scope_(
          v8::Local<v8::Context>::New(client.isolate_, client.context_)),
      provider_(client.provider_.NewHandle()),
      spell_check_(client.spell_check_.NewHandle()) {}

SpellCheckClient::SpellCheckScope::~SpellCheckScope() = default;

}  // namespace api

}  // namespace electron
