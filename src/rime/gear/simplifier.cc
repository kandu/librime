//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-12-12 GONG Chen <chen.sst@gmail.com>
//
#include <boost/algorithm/string.hpp>
#include <stdint.h>
#include <utf8.h>
#include <utility>
#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/translation.h>
#include <rime/gear/simplifier.h>
#include <opencc/Config.hpp>  // Place OpenCC #includes here to avoid VS2015 compilation errors
#include <opencc/Converter.hpp>
#include <opencc/Conversion.hpp>
#include <opencc/ConversionChain.hpp>
#include <opencc/Dict.hpp>
#include <opencc/DictEntry.hpp>

static const char* quote_left = "\xe3\x80\x94";   //"\xef\xbc\x88";
static const char* quote_right = "\xe3\x80\x95";  //"\xef\xbc\x89";

namespace rime {

class Opencc {
 public:
  Opencc(const path& config_path) {
    LOG(INFO) << "initializing opencc: " << config_path;
    opencc::Config config;
    try {
      // opencc accepts file path encoded in UTF-8.
      converter_ = config.NewFromFile(config_path.u8string());

      const list<opencc::ConversionPtr> conversions =
          converter_->GetConversionChain()->GetConversions();
      dict_ = conversions.front()->GetDict();
    } catch (...) {
      LOG(ERROR) << "opencc config not found: " << config_path;
    }
  }

  bool ConvertWord(const string& text, vector<string>* forms) {
    if (converter_ == nullptr) {
      return false;
    }
    const list<opencc::ConversionPtr> conversions =
        converter_->GetConversionChain()->GetConversions();
    vector<string> original_words{text};
    bool matched = false;
    for (auto conversion : conversions) {
      opencc::DictPtr dict = conversion->GetDict();
      if (dict == nullptr) {
        return false;
      }
      set<string> word_set;
      vector<string> converted_words;
      for (const auto& original_word : original_words) {
        opencc::Optional<const opencc::DictEntry*> item =
            dict->Match(original_word);
        if (item.IsNull()) {
          // There is no exact match, but still need to convert partially
          // matched in a chain conversion. Here apply default (max. seg.)
          // match to get the most probable conversion result
          std::ostringstream buffer;
          for (const char* wstr = original_word.c_str(); *wstr != '\0';) {
            opencc::Optional<const opencc::DictEntry*> matched =
                dict->MatchPrefix(wstr);
            size_t matched_length;
            if (matched.IsNull()) {
              matched_length = opencc::UTF8Util::NextCharLength(wstr);
              buffer << opencc::UTF8Util::FromSubstr(wstr, matched_length);
            } else {
              matched_length = matched.Get()->KeyLength();
              buffer << matched.Get()->GetDefault();
            }
            wstr += matched_length;
          }
          const string& converted_word = buffer.str();
          // Even if current dictionary doesn't convert the word
          // (converted_word == original_word), we still need to keep it for
          // subsequent dicts in the chain. e.g. s2t.json expands 里 to 里 and
          // 裏, then t2tw.json passes 里 as-is and converts 裏 to 裡.
          if (word_set.insert(converted_word).second) {
            converted_words.push_back(converted_word);
          }
          continue;
        }
        matched = true;
        const opencc::DictEntry* entry = item.Get();
        for (const auto& converted_word : entry->Values()) {
          if (word_set.insert(converted_word).second) {
            converted_words.push_back(converted_word);
          }
        }
      }
      original_words.swap(converted_words);
    }
    if (!matched) {
      // No dictionary contains the word
      return false;
    }
    *forms = std::move(original_words);
    return forms->size() > 0;
  }

  bool RandomConvertText(const string& text, string* simplified) {
    if (dict_ == nullptr)
      return false;
    const list<opencc::ConversionPtr> conversions =
        converter_->GetConversionChain()->GetConversions();
    const char* phrase = text.c_str();
    for (auto conversion : conversions) {
      opencc::DictPtr dict = conversion->GetDict();
      if (dict == nullptr) {
        return false;
      }
      std::ostringstream buffer;
      for (const char* pstr = phrase; *pstr != '\0';) {
        opencc::Optional<const opencc::DictEntry*> matched =
            dict->MatchPrefix(pstr);
        size_t matched_length;
        if (matched.IsNull()) {
          matched_length = opencc::UTF8Util::NextCharLength(pstr);
          buffer << opencc::UTF8Util::FromSubstr(pstr, matched_length);
        } else {
          matched_length = matched.Get()->KeyLength();
          size_t i = rand() % (matched.Get()->NumValues());
          buffer << matched.Get()->Values().at(i);
        }
        pstr += matched_length;
      }
      *simplified = buffer.str();
      phrase = simplified->c_str();
    }
    return *simplified != text;
  }

  bool ConvertText(const string& text, string* simplified) {
    if (converter_ == nullptr)
      return false;
    *simplified = converter_->Convert(text);
    return *simplified != text;
  }

 private:
  opencc::ConverterPtr converter_;
  opencc::DictPtr dict_;
};

// Simplifier

Simplifier::Simplifier(const Ticket& ticket, an<Opencc> opencc)
    : Filter(ticket), TagMatching(ticket), opencc_(opencc) {
  if (name_space_ == "filter") {
    name_space_ = "simplifier";
  }
  if (Config* config = engine_->schema()->config()) {
    string tips;
    if (config->GetString(name_space_ + "/tips", &tips) ||
        config->GetString(name_space_ + "/tip", &tips)) {
      tips_level_ = (tips == "all")    ? kTipsAll
                    : (tips == "char") ? kTipsChar
                                       : kTipsNone;
    }
    config->GetBool(name_space_ + "/show_in_comment", &show_in_comment_);
    config->GetBool(name_space_ + "/inherit_comment", &inherit_comment_);
    comment_formatter_.Load(config->GetList(name_space_ + "/comment_format"));
    config->GetBool(name_space_ + "/random", &random_);
    config->GetString(name_space_ + "/option_name", &option_name_);
    if (auto types = config->GetList(name_space_ + "/excluded_types")) {
      for (auto it = types->begin(); it != types->end(); ++it) {
        if (auto value = As<ConfigValue>(*it)) {
          excluded_types_.insert(value->str());
        }
      }
    }
  }
  if (option_name_.empty()) {
    option_name_ = "simplification";  // default switcher option
  }
  if (random_) {
    srand((unsigned)time(NULL));
  }
}

class SimplifiedTranslation : public PrefetchTranslation {
 public:
  SimplifiedTranslation(an<Translation> translation, Simplifier* simplifier)
      : PrefetchTranslation(translation), simplifier_(simplifier) {}

 protected:
  virtual bool Replenish();

  Simplifier* simplifier_;
};

bool SimplifiedTranslation::Replenish() {
  auto next = translation_->Peek();
  translation_->Next();
  if (next && !simplifier_->Convert(next, &cache_)) {
    cache_.push_back(next);
  }
  return !cache_.empty();
}

an<Translation> Simplifier::Apply(an<Translation> translation,
                                  CandidateList* candidates) {
  if (!engine_->context()->get_option(option_name_)) {  // off
    return translation;
  }
  if (!opencc_) {
    return translation;
  }
  return New<SimplifiedTranslation>(translation, this);
}

void Simplifier::PushBack(const an<Candidate>& original,
                          CandidateQueue* result,
                          const string& simplified) {
  string tips;
  string text;
  size_t length = utf8::unchecked::distance(
      original->text().c_str(),
      original->text().c_str() + original->text().length());
  bool show_tips =
      (tips_level_ == kTipsChar && length == 1) || tips_level_ == kTipsAll;
  if (show_in_comment_) {
    text = original->text();
    if (show_tips) {
      tips = simplified;
      comment_formatter_.Apply(&tips);
    }
  } else {
    text = simplified;
    if (show_tips) {
      tips = original->text();
      bool modified = comment_formatter_.Apply(&tips);
      if (!modified) {
        tips = quote_left + original->text() + quote_right;
      }
    }
  }
  result->push_back(New<ShadowCandidate>(original, "simplified", text, tips,
                                         inherit_comment_));
}

bool Simplifier::Convert(const an<Candidate>& original,
                         CandidateQueue* result) {
  if (excluded_types_.find(original->type()) != excluded_types_.end()) {
    return false;
  }
  bool success = false;
  if (random_) {
    string simplified;
    success = opencc_->RandomConvertText(original->text(), &simplified);
    if (success) {
      PushBack(original, result, simplified);
    }
  } else {  //! random_
    vector<string> forms;
    success = opencc_->ConvertWord(original->text(), &forms);
    if (success) {
      for (size_t i = 0; i < forms.size(); ++i) {
        if (forms[i] == original->text()) {
          result->push_back(original);
        } else {
          PushBack(original, result, forms[i]);
        }
      }
    } else {
      string simplified;
      success = opencc_->ConvertText(original->text(), &simplified);
      if (success) {
        PushBack(original, result, simplified);
      }
    }
  }
  return success;
}

SimplifierComponent::SimplifierComponent() {}

Simplifier* SimplifierComponent::Create(const Ticket& ticket) {
  string name_space = ticket.name_space;
  if (name_space == "filter") {
    name_space = "simplifier";
  }
  string opencc_config;
  an<Opencc> opencc;
  if (Config* config = ticket.engine->schema()->config()) {
    config->GetString(name_space + "/opencc_config", &opencc_config);
  }
  if (opencc_config.empty()) {
    opencc_config = "t2s.json";  // default opencc config file
  }
  opencc = opencc_map_[opencc_config].lock();
  if (opencc) {
    return new Simplifier(ticket, opencc);
  }
  path opencc_config_path = path(opencc_config);
  if (opencc_config_path.extension().u8string() == ".ini") {
    LOG(ERROR) << "please upgrade opencc_config to an opencc 1.0 config file.";
    return nullptr;
  }
  if (opencc_config_path.is_relative()) {
    path user_config_path = Service::instance().deployer().user_data_dir;
    path shared_config_path = Service::instance().deployer().shared_data_dir;
    (user_config_path /= "opencc") /= opencc_config_path;
    (shared_config_path /= "opencc") /= opencc_config_path;
    if (exists(user_config_path)) {
      opencc_config_path = user_config_path;
    } else if (exists(shared_config_path)) {
      opencc_config_path = shared_config_path;
    }
  }
  try {
    opencc = New<Opencc>(opencc_config_path);
    // 以原始配置中的文件路径作为 key，避免重复查找文件
    opencc_map_[opencc_config] = opencc;
  } catch (opencc::Exception& e) {
    LOG(ERROR) << "Error initializing opencc: " << e.what();
    return nullptr;
  }
  return new Simplifier(ticket, opencc);
}

}  // namespace rime
