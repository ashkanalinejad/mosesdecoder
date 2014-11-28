// $Id$
// vim:tabstop=2  

/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

#include <string>

#include "TypeDef.h"
#include "moses/FF/WordPenaltyProducer.h"
#include "moses/FF/UnknownWordPenaltyProducer.h"
#include "moses/FF/InputFeature.h"

#include "DecodeStepTranslation.h"
#include "DecodeStepGeneration.h"
#include "GenerationDictionary.h"
#include "StaticData.h"
#include "Util.h"
#include "FactorCollection.h"
#include "Timer.h"
#include "UserMessage.h"
#include "TranslationOption.h"
#include "DecodeGraph.h"
#include "InputFileStream.h"
#include "ScoreComponentCollection.h"
#include "DecodeGraph.h"
#include "TranslationModel/PhraseDictionary.h"
#include "TranslationModel/PhraseDictionaryTreeAdaptor.h"

#ifdef WITH_THREADS
#include <boost/thread.hpp>
#endif

using namespace std;

namespace Moses
{
bool g_mosesDebug = false;

StaticData StaticData::s_instance;

StaticData::StaticData()
  :m_sourceStartPosMattersForRecombination(false)
  ,m_inputType(SentenceInput)
  ,m_onlyDistinctNBest(false)
  ,m_needAlignmentInfo(false)
  ,m_lmEnableOOVFeature(false)
  ,m_isAlwaysCreateDirectTranslationOption(false)
  ,m_currentWeightSetting("default")
  ,m_treeStructure(NULL)
  ,m_useS2TDecoder(false)
{
  m_xmlBrackets.first="<";
  m_xmlBrackets.second=">";

  // memory pools
  Phrase::InitializeMemPool();
}

StaticData::~StaticData()
{
  RemoveAllInColl(m_decodeGraphs);

  /*
  const std::vector<FeatureFunction*> &producers = FeatureFunction::GetFeatureFunctions();
  for(size_t i=0;i<producers.size();++i) {
  FeatureFunction *ff = producers[i];
    delete ff;
  }
  */

  // memory pools
  Phrase::FinalizeMemPool();
}

bool StaticData::LoadDataStatic(Parameter *parameter, const std::string &execPath)
{
  s_instance.SetExecPath(execPath);
  return s_instance.LoadData(parameter);
}

bool StaticData::LoadData(Parameter *parameter)
{
  ResetUserTime();
  m_parameter = parameter;

  const PARAM_VEC *params;

  // verbose level
  m_parameter->SetParameter(m_verboseLevel, "verbose", (size_t) 1);

  // to cube or not to cube
  m_parameter->SetParameter(m_searchAlgorithm, "search-algorithm", Normal);

  if (IsChart())
    LoadChartDecodingParameters();

  // input type has to be specified BEFORE loading the phrase tables!
  m_parameter->SetParameter(m_inputType, "inputtype", SentenceInput);

  std::string s_it = "text input";
  if (m_inputType == 1) {
    s_it = "confusion net";
  }
  if (m_inputType == 2) {
    s_it = "word lattice";
  }
  if (m_inputType == 3) {
    s_it = "tree";
  }
  VERBOSE(2,"input type is: "<<s_it<<"\n");

  m_parameter->SetParameter(m_recoverPath, "recover-input-path", false);
  if (m_recoverPath && m_inputType == SentenceInput) {
      TRACE_ERR("--recover-input-path should only be used with confusion net or word lattice input!\n");
      m_recoverPath = false;
    }

  // factor delimiter
  m_parameter->SetParameter<string>(m_factorDelimiter, "factor-delimiter", "|");
  if (m_factorDelimiter == "none") {
      m_factorDelimiter = "";
  }

  SetBooleanParameter( m_continuePartialTranslation, "continue-partial-translation", false );
  SetBooleanParameter( m_outputHypoScore, "output-hypo-score", false );

  //word-to-word alignment
  // alignments
  SetBooleanParameter( m_PrintAlignmentInfo, "print-alignment-info", false );
  if (m_PrintAlignmentInfo) {
    m_needAlignmentInfo = true;
  }

  m_parameter->SetParameter(m_wordAlignmentSort, "sort-word-alignment", NoSort);

  SetBooleanParameter( m_PrintAlignmentInfoNbest, "print-alignment-info-in-n-best", false );
  if (m_PrintAlignmentInfoNbest) {
    m_needAlignmentInfo = true;
  }

  params = m_parameter->GetParam("alignment-output-file");
  if (params && params->size()) {
    m_alignmentOutputFile = Scan<std::string>(params->at(0));
    m_needAlignmentInfo = true;
  }

  // n-best
  params = m_parameter->GetParam("n-best-list");
  if (params) {
	  if (params->size() >= 2) {
		m_nBestFilePath = params->at(0);
		m_nBestSize = Scan<size_t>( params->at(1) );
		m_onlyDistinctNBest=(params->size()>2 && params->at(2)=="distinct");
	  }
	  else {
		  UserMessage::Add(string("wrong format for switch -n-best-list file size [disinct]"));
		  return false;
	  }
  } else {
    m_nBestSize = 0;
  }

  m_parameter->SetParameter<size_t>(m_nBestFactor, "n-best-factor", 20);

  //lattice samples
  params = m_parameter->GetParam("lattice-samples");
  if (params) {
	  if (params->size() ==2 ) {
		m_latticeSamplesFilePath = params->at(0);
		m_latticeSamplesSize = Scan<size_t>(params->at(1));
	  }
	  else {
		UserMessage::Add(string("wrong format for switch -lattice-samples file size"));
		return false;
	  }
  }
  else {
    m_latticeSamplesSize = 0;
  }

  // word graph
  params = m_parameter->GetParam("output-word-graph");
  if (params && params->size() == 2)
	m_outputWordGraph = true;
  else
	m_outputWordGraph = false;

  // search graph
  params = m_parameter->GetParam("output-search-graph");
  if (params && params->size()) {
    if (params->size() != 1) {
      UserMessage::Add(string("ERROR: wrong format for switch -output-search-graph file"));
      return false;
    }
    m_outputSearchGraph = true;
  }
  // ... in extended format
  else if (m_parameter->GetParam("output-search-graph-extended") &&
		  m_parameter->GetParam("output-search-graph-extended")->size()) {
    if (m_parameter->GetParam("output-search-graph-extended")->size() != 1) {
      UserMessage::Add(string("ERROR: wrong format for switch -output-search-graph-extended file"));
      return false;
    }
    m_outputSearchGraph = true;
    m_outputSearchGraphExtended = true;
  } else {
    m_outputSearchGraph = false;
  }

  params = m_parameter->GetParam("output-search-graph-slf");
  if (params && params->size()) {
    m_outputSearchGraphSLF = true;
  } else {
    m_outputSearchGraphSLF = false;
  }

  params = m_parameter->GetParam("output-search-graph-hypergraph");
  if (params && params->size()) {
    m_outputSearchGraphHypergraph = true;
  } else {
    m_outputSearchGraphHypergraph = false;
  }

#ifdef HAVE_PROTOBUF
  params = m_parameter->GetParam("output-search-graph-pb");
  if (params && params->size()) {
    if (params->size() != 1) {
      UserMessage::Add(string("ERROR: wrong format for switch -output-search-graph-pb path"));
      return false;
    }
    m_outputSearchGraphPB = true;
  } else
    m_outputSearchGraphPB = false;
#endif

  SetBooleanParameter( m_unprunedSearchGraph, "unpruned-search-graph", false );
  SetBooleanParameter( m_includeLHSInSearchGraph, "include-lhs-in-search-graph", false );

  m_parameter->SetParameter<string>(m_outputUnknownsFile, "output-unknowns", "");

  // include feature names in the n-best list
  SetBooleanParameter( m_labeledNBestList, "labeled-n-best-list", true );

  // include word alignment in the n-best list
  SetBooleanParameter( m_nBestIncludesSegmentation, "include-segmentation-in-n-best", false );

  // printing source phrase spans
  SetBooleanParameter( m_reportSegmentation, "report-segmentation", false );
  SetBooleanParameter( m_reportSegmentationEnriched, "report-segmentation-enriched", false );

  // print all factors of output translations
  SetBooleanParameter( m_reportAllFactors, "report-all-factors", false );

  // print all factors of output translations
  SetBooleanParameter( m_reportAllFactorsNBest, "report-all-factors-in-n-best", false );

  //input factors
  params = m_parameter->GetParam("input-factors");
  if (params) {
	m_inputFactorOrder = Scan<FactorType>(*params);
  }
  if(m_inputFactorOrder.empty()) {
	m_inputFactorOrder.push_back(0);
  }

  //output factors
  params = m_parameter->GetParam("output-factors");
  if (params) {
	  m_outputFactorOrder = Scan<FactorType>(*params);
  }
  if(m_outputFactorOrder.empty()) {
    // default. output factor 0
    m_outputFactorOrder.push_back(0);
  }

  //source word deletion
  SetBooleanParameter(m_wordDeletionEnabled, "phrase-drop-allowed", false );

  //Disable discarding
  SetBooleanParameter(m_disableDiscarding, "disable-discarding", false);

  //Print All Derivations
  SetBooleanParameter(m_printAllDerivations , "print-all-derivations", false );

  // additional output
  m_parameter->SetParameter<string>(m_detailedTranslationReportingFilePath, "translation-details", "");
  m_parameter->SetParameter<string>(m_detailedTreeFragmentsTranslationReportingFilePath, "tree-translation-details", "");

  //DIMw
  m_parameter->SetParameter<string>(m_detailedAllTranslationReportingFilePath, "translation-all-details", "");

  // reordering constraints
  m_parameter->SetParameter(m_maxDistortion, "distortion-limit", -1);

  SetBooleanParameter(m_reorderingConstraint, "monotone-at-punctuation", false );

  // settings for pruning
  m_parameter->SetParameter(m_maxHypoStackSize, "stack", DEFAULT_MAX_HYPOSTACK_SIZE);

  m_minHypoStackDiversity = 0;
  params = m_parameter->GetParam("stack-diversity");
  if (params && params->size()) {
    if (m_maxDistortion > 15) {
      UserMessage::Add("stack diversity > 0 is not allowed for distortion limits larger than 15");
      return false;
    }
    if (m_inputType == WordLatticeInput) {
      UserMessage::Add("stack diversity > 0 is not allowed for lattice input");
      return false;
    }
    m_minHypoStackDiversity = Scan<size_t>(params->at(0));
  }

  m_parameter->SetParameter(m_beamWidth, "beam-threshold", DEFAULT_BEAM_WIDTH);
  m_beamWidth = TransformScore(m_beamWidth);

  m_parameter->SetParameter(m_earlyDiscardingThreshold, "early-discarding-threshold", DEFAULT_EARLY_DISCARDING_THRESHOLD);
  m_earlyDiscardingThreshold = TransformScore(m_earlyDiscardingThreshold);

  m_parameter->SetParameter(m_translationOptionThreshold, "translation-option-threshold", DEFAULT_TRANSLATION_OPTION_THRESHOLD);
  m_translationOptionThreshold = TransformScore(m_translationOptionThreshold);

  m_parameter->SetParameter(m_maxNoTransOptPerCoverage, "max-trans-opt-per-coverage", DEFAULT_MAX_TRANS_OPT_SIZE);
  m_parameter->SetParameter(m_maxNoPartTransOpt, "max-partial-trans-opt", DEFAULT_MAX_PART_TRANS_OPT_SIZE);
  m_parameter->SetParameter(m_maxPhraseLength, "max-phrase-length", DEFAULT_MAX_PHRASE_LENGTH);
  m_parameter->SetParameter(m_cubePruningPopLimit, "cube-pruning-pop-limit", DEFAULT_CUBE_PRUNING_POP_LIMIT);
  m_parameter->SetParameter(m_cubePruningDiversity, "cube-pruning-diversity", DEFAULT_CUBE_PRUNING_DIVERSITY);

  SetBooleanParameter(m_cubePruningLazyScoring, "cube-pruning-lazy-scoring", false);

  // early distortion cost
  SetBooleanParameter(m_useEarlyDistortionCost, "early-distortion-cost", false );

  // unknown word processing
  SetBooleanParameter(m_dropUnknown, "drop-unknown", false );
  SetBooleanParameter(m_markUnknown, "mark-unknown", false );

  SetBooleanParameter(m_lmEnableOOVFeature, "lmodel-oov-feature", false);

  // minimum Bayes risk decoding
  SetBooleanParameter(m_mbr, "minimum-bayes-risk", false );
  m_parameter->SetParameter<size_t>(m_mbrSize, "mbr-size", 200);
  m_parameter->SetParameter(m_mbrScale, "mbr-scale", 1.0f);

  //lattice mbr
  SetBooleanParameter(m_useLatticeMBR, "lminimum-bayes-risk", false );
  if (m_useLatticeMBR && m_mbr) {
    cerr << "Errror: Cannot use both n-best mbr and lattice mbr together" << endl;
    return false;
  }

  //mira training
  SetBooleanParameter(m_mira, "mira", false );

  // lattice MBR
  if (m_useLatticeMBR) m_mbr = true;

  m_parameter->SetParameter<size_t>(m_lmbrPruning, "lmbr-pruning-factor", 30);
  m_parameter->SetParameter(m_lmbrPrecision, "lmbr-p", 0.8f);
  m_parameter->SetParameter(m_lmbrPRatio, "lmbr-r", 0.6f);
  m_parameter->SetParameter(m_lmbrMapWeight, "lmbr-map-weight", 0.0f);
  SetBooleanParameter(m_useLatticeHypSetForLatticeMBR, "lattice-hypo-set", false );

  params = m_parameter->GetParam("lmbr-thetas");
  if (params) {
	  m_lmbrThetas = Scan<float>(*params);
  }

  //consensus decoding
  SetBooleanParameter(m_useConsensusDecoding, "consensus-decoding", false );
  if (m_useConsensusDecoding && m_mbr) {
    cerr<< "Error: Cannot use consensus decoding together with mbr" << endl;
    exit(1);
  }
  if (m_useConsensusDecoding) m_mbr=true;

  SetBooleanParameter(m_defaultNonTermOnlyForEmptyRange, "default-non-term-for-empty-range-only", false );
  SetBooleanParameter(m_printNBestTrees, "n-best-trees", false );

  // S2T decoder
  SetBooleanParameter(m_useS2TDecoder, "s2t", false );
  m_parameter->SetParameter(m_s2tParsingAlgorithm, "s2t-parsing-algorithm", RecursiveCYKPlus);

  // Compact phrase table and reordering model
  SetBooleanParameter(m_minphrMemory, "minphr-memory", false );
  SetBooleanParameter(m_minlexrMemory, "minlexr-memory", false );

  m_parameter->SetParameter<size_t>(m_timeout_threshold, "time-out", -1);
  m_timeout = (GetTimeoutThreshold() == (size_t)-1) ? false : true;

  m_parameter->SetParameter<size_t>(m_lmcache_cleanup_threshold, "clean-lm-cache", 1);

  m_threadCount = 1;
  params = m_parameter->GetParam("threads");
  if (params && params->size()) {
    if (params->at(0) == "all") {
#ifdef WITH_THREADS
      m_threadCount = boost::thread::hardware_concurrency();
      if (!m_threadCount) {
        UserMessage::Add("-threads all specified but Boost doesn't know how many cores there are");
        return false;
      }
#else
      UserMessage::Add("-threads all specified but moses not built with thread support");
      return false;
#endif
    } else {
      m_threadCount = Scan<int>(params->at(0));
      if (m_threadCount < 1) {
        UserMessage::Add("Specify at least one thread.");
        return false;
      }
#ifndef WITH_THREADS
      if (m_threadCount > 1) {
        UserMessage::Add(std::string("Error: Thread count of ") + params->at(0) + " but moses not built with thread support");
        return false;
      }
#endif
    }
  }

  m_parameter->SetParameter<long>(m_startTranslationId, "start-translation-id", 0);

  // use of xml in input
  m_parameter->SetParameter<XmlInputType>(m_xmlInputType, "xml-input", XmlPassThrough);

  // specify XML tags opening and closing brackets for XML option
  params = m_parameter->GetParam("xml-brackets");
  if (params && params->size()) {
    std::vector<std::string> brackets = Tokenize(params->at(0));
    if(brackets.size()!=2) {
      cerr << "invalid xml-brackets value, must specify exactly 2 blank-delimited strings for XML tags opening and closing brackets" << endl;
      exit(1);
    }
    m_xmlBrackets.first= brackets[0];
    m_xmlBrackets.second=brackets[1];
    VERBOSE(1,"XML tags opening and closing brackets for XML input are: " 
	    << m_xmlBrackets.first << " and " << m_xmlBrackets.second << endl);
  }

  m_parameter->SetParameter(m_placeHolderFactor, "placeholder-factor", NOT_FOUND);

  std::map<std::string, std::string> featureNameOverride = OverrideFeatureNames();

  // all features
  map<string, int> featureIndexMap;

  params = m_parameter->GetParam("feature");
  for (size_t i = 0; params && i < params->size(); ++i) {
    const string &line = Trim(params->at(i));
    VERBOSE(1,"line=" << line << endl);
    if (line.empty())
      continue;

    vector<string> toks = Tokenize(line);

    string &feature = toks[0];
    std::map<std::string, std::string>::const_iterator iter = featureNameOverride.find(feature);
    if (iter == featureNameOverride.end()) {
    	// feature name not override
    	m_registry.Construct(feature, line);
    }
    else {
    	// replace feature name with new name
    	string newName = iter->second;
    	feature = newName;
    	string newLine = Join(" ", toks);
    	m_registry.Construct(newName, newLine);
    }
  }

  NoCache();
  OverrideFeatures();

  if (m_parameter->GetParam("show-weights") == NULL) {
    LoadFeatureFunctions();
  }

  if (!LoadDecodeGraphs()) return false;


  if (!CheckWeights()) {
    return false;
  }

  //Add any other features here.

  //Load extra feature weights
  string weightFile;
  m_parameter->SetParameter<string>(weightFile, "weight-file", "");
  if (!weightFile.empty()) {
    ScoreComponentCollection extraWeights;
    if (!extraWeights.Load(weightFile)) {
      UserMessage::Add("Unable to load weights from " + weightFile);
      return false;
    }
    m_allWeights.PlusEquals(extraWeights);
  }

  //Load sparse features from config (overrules weight file)
  LoadSparseWeightsFromConfig();

  // alternate weight settings
  params = m_parameter->GetParam("alternate-weight-setting");
  if (params && params->size()) {
    if (!LoadAlternateWeightSettings()) {
      return false;
    }
  }
  return true;
}

void StaticData::SetBooleanParameter( bool &parameter, string parameterName, bool defaultValue )
{
  const PARAM_VEC *params = m_parameter->GetParam(parameterName);

  // default value if nothing is specified
  parameter = defaultValue;
  if (params == NULL) {
    return;
  }

  // if parameter is just specified as, e.g. "-parameter" set it true
  if (params->size() == 0) {
    parameter = true;
  }
  // if paramter is specified "-parameter true" or "-parameter false"
  else if (params->size() == 1) {
    parameter = Scan<bool>( params->at(0));
  }
}

void StaticData::SetWeight(const FeatureFunction* sp, float weight)
{
  m_allWeights.Resize();
  m_allWeights.Assign(sp,weight);
}

void StaticData::SetWeights(const FeatureFunction* sp, const std::vector<float>& weights)
{
  m_allWeights.Resize();
  m_allWeights.Assign(sp,weights);
}

void StaticData::LoadNonTerminals()
{
  string defaultNonTerminals;
  m_parameter->SetParameter<string>(defaultNonTerminals, "non-terminals", "X");

  FactorCollection &factorCollection = FactorCollection::Instance();

  m_inputDefaultNonTerminal.SetIsNonTerminal(true);
  const Factor *sourceFactor = factorCollection.AddFactor(Input, 0, defaultNonTerminals, true);
  m_inputDefaultNonTerminal.SetFactor(0, sourceFactor);

  m_outputDefaultNonTerminal.SetIsNonTerminal(true);
  const Factor *targetFactor = factorCollection.AddFactor(Output, 0, defaultNonTerminals, true);
  m_outputDefaultNonTerminal.SetFactor(0, targetFactor);

  // for unknown words
  const PARAM_VEC *params = m_parameter->GetParam("unknown-lhs");
  if (params == NULL || params->size() == 0) {
    UnknownLHSEntry entry(defaultNonTerminals, 0.0f);
    m_unknownLHS.push_back(entry);
  } else {
    const string &filePath = params->at(0);

    InputFileStream inStream(filePath);
    string line;
    while(getline(inStream, line)) {
      vector<string> tokens = Tokenize(line);
      UTIL_THROW_IF2(tokens.size() != 2,
    		  "Incorrect unknown LHS format: " << line);
      UnknownLHSEntry entry(tokens[0], Scan<float>(tokens[1]));
      m_unknownLHS.push_back(entry);
      // const Factor *targetFactor = 
      factorCollection.AddFactor(Output, 0, tokens[0], true);
    }

  }

}

void StaticData::LoadChartDecodingParameters()
{
  LoadNonTerminals();

  // source label overlap
  m_parameter->SetParameter(m_sourceLabelOverlap, "source-label-overlap", SourceLabelOverlapAdd);
  m_parameter->SetParameter(m_ruleLimit, "rule-limit", DEFAULT_MAX_TRANS_OPT_SIZE);

}

bool StaticData::LoadDecodeGraphs()
{
  vector<string> mappingVector;
  vector<size_t> maxChartSpans;

  const PARAM_VEC *params;

  params = m_parameter->GetParam("mapping");
  if (params && params->size()) {
	  mappingVector = *params;
  }

  params = m_parameter->GetParam("max-chart-span");
  if (params && params->size()) {
	  maxChartSpans = Scan<size_t>(*params);
  }

  const vector<PhraseDictionary*>& pts = PhraseDictionary::GetColl();
  const vector<GenerationDictionary*>& gens = GenerationDictionary::GetColl();

  const std::vector<FeatureFunction*> *featuresRemaining = &FeatureFunction::GetFeatureFunctions();
  DecodeStep *prev = 0;
  size_t prevDecodeGraphInd = 0;

  for(size_t i=0; i<mappingVector.size(); i++) {
    vector<string>	token		= Tokenize(mappingVector[i]);
    size_t decodeGraphInd;
    DecodeType decodeType;
    size_t index;
    if (token.size() == 2) {
      decodeGraphInd = 0;
      decodeType = token[0] == "T" ? Translate : Generate;
      index = Scan<size_t>(token[1]);
    } else if (token.size() == 3) {
      // For specifying multiple translation model
      decodeGraphInd = Scan<size_t>(token[0]);
      //the vectorList index can only increment by one
      UTIL_THROW_IF2(decodeGraphInd != prevDecodeGraphInd && decodeGraphInd != prevDecodeGraphInd + 1,
    		  "Malformed mapping");
      if (decodeGraphInd > prevDecodeGraphInd) {
        prev = NULL;
      }

      if (prevDecodeGraphInd < decodeGraphInd) {
        featuresRemaining = &FeatureFunction::GetFeatureFunctions();
      }

      decodeType = token[1] == "T" ? Translate : Generate;
      index = Scan<size_t>(token[2]);
    } else {
      UTIL_THROW(util::Exception, "Malformed mapping");
    }

    DecodeStep* decodeStep = NULL;
    switch (decodeType) {
    case Translate:
      if(index>=pts.size()) {
        stringstream strme;
        strme << "No phrase dictionary with index "
              << index << " available!";
        UTIL_THROW(util::Exception, strme.str());
      }
      decodeStep = new DecodeStepTranslation(pts[index], prev, *featuresRemaining);
      break;
    case Generate:
      if(index>=gens.size()) {
        stringstream strme;
        strme << "No generation dictionary with index "
              << index << " available!";
        UTIL_THROW(util::Exception, strme.str());
      }
      decodeStep = new DecodeStepGeneration(gens[index], prev, *featuresRemaining);
      break;
    case InsertNullFertilityWord:
      UTIL_THROW(util::Exception, "Please implement NullFertilityInsertion.");
      break;
    }

    featuresRemaining = &decodeStep->GetFeaturesRemaining();

    UTIL_THROW_IF2(decodeStep == NULL, "Null decode step");
    if (m_decodeGraphs.size() < decodeGraphInd + 1) {
      DecodeGraph *decodeGraph;
      if (IsChart()) {
        size_t maxChartSpan = (decodeGraphInd < maxChartSpans.size()) ? maxChartSpans[decodeGraphInd] : DEFAULT_MAX_CHART_SPAN;
        VERBOSE(1,"max-chart-span: " << maxChartSpans[decodeGraphInd] << endl);
        decodeGraph = new DecodeGraph(m_decodeGraphs.size(), maxChartSpan);
      } else {
        decodeGraph = new DecodeGraph(m_decodeGraphs.size());
      }

      m_decodeGraphs.push_back(decodeGraph); // TODO max chart span
    }

    m_decodeGraphs[decodeGraphInd]->Add(decodeStep);
    prev = decodeStep;
    prevDecodeGraphInd = decodeGraphInd;
  }

  // set maximum n-gram size for backoff approach to decoding paths
  // default is always use subsequent paths (value = 0)
  // if specified, record maxmimum unseen n-gram size
  const vector<string> *backoffVector = m_parameter->GetParam("decoding-graph-backoff");
  for(size_t i=0; i<m_decodeGraphs.size() && backoffVector && i<backoffVector->size(); i++) {
	DecodeGraph &decodeGraph = *m_decodeGraphs[i];

	if (i < backoffVector->size()) {
		decodeGraph.SetBackoff(Scan<size_t>(backoffVector->at(i)));
	}
  }

  return true;
}

void StaticData::ReLoadParameter()
{
  UTIL_THROW(util::Exception, "completely redo. Too many hardcoded ff"); // TODO completely redo. Too many hardcoded ff
  /*
  m_verboseLevel = 1;
  if (m_parameter->GetParam("verbose").size() == 1) {
    m_verboseLevel = Scan<size_t>( m_parameter->GetParam("verbose")[0]);
  }

  // check whether "weight-u" is already set
  if (m_parameter->isParamShortNameSpecified("u")) {
    if (m_parameter->GetParamShortName("u").size() < 1 ) {
      PARAM_VEC w(1,"1.0");
      m_parameter->OverwriteParamShortName("u", w);
    }
  }

  //loop over all ScoreProducer to update weights

  std::vector<const ScoreProducer*>::const_iterator iterSP;
  for (iterSP = transSystem.GetFeatureFunctions().begin() ; iterSP != transSystem.GetFeatureFunctions().end() ; ++iterSP) {
    std::string paramShortName = (*iterSP)->GetScoreProducerWeightShortName();
    vector<float> Weights = Scan<float>(m_parameter->GetParamShortName(paramShortName));

    if (paramShortName == "d") { //basic distortion model takes the first weight
      if ((*iterSP)->GetScoreProducerDescription() == "Distortion") {
        Weights.resize(1); //take only the first element
      } else { //lexicalized reordering model takes the other
        Weights.erase(Weights.begin()); //remove the first element
      }
      //			std::cerr << "this is the Distortion Score Producer -> " << (*iterSP)->GetScoreProducerDescription() << std::cerr;
      //			std::cerr << "this is the Distortion Score Producer; it has " << (*iterSP)->GetNumScoreComponents() << " weights"<< std::cerr;
      //  	std::cerr << Weights << std::endl;
    } else if (paramShortName == "tm") {
      continue;
    }
    SetWeights(*iterSP, Weights);
  }

  //	std::cerr << "There are " << m_phraseDictionary.size() << " m_phraseDictionaryfeatures" << std::endl;

  const vector<float> WeightsTM = Scan<float>(m_parameter->GetParamShortName("tm"));
  //  std::cerr << "WeightsTM: " << WeightsTM << std::endl;

  const vector<float> WeightsLM = Scan<float>(m_parameter->GetParamShortName("lm"));
  //  std::cerr << "WeightsLM: " << WeightsLM << std::endl;

  size_t index_WeightTM = 0;
  for(size_t i=0; i<transSystem.GetPhraseDictionaries().size(); ++i) {
    PhraseDictionaryFeature &phraseDictionaryFeature = *m_phraseDictionary[i];

    //		std::cerr << "phraseDictionaryFeature.GetNumScoreComponents():" << phraseDictionaryFeature.GetNumScoreComponents() << std::endl;
    //		std::cerr << "phraseDictionaryFeature.GetNumInputScores():" << phraseDictionaryFeature.GetNumInputScores() << std::endl;

    vector<float> tmp_weights;
    for(size_t j=0; j<phraseDictionaryFeature.GetNumScoreComponents(); ++j)
      tmp_weights.push_back(WeightsTM[index_WeightTM++]);

    //  std::cerr << tmp_weights << std::endl;

    SetWeights(&phraseDictionaryFeature, tmp_weights);
  }
  */
}

void StaticData::ReLoadBleuScoreFeatureParameter(float weight)
{
  assert(false);
  /*
  //loop over ScoreProducers to update weights of BleuScoreFeature

  std::vector<const ScoreProducer*>::const_iterator iterSP;
  for (iterSP = transSystem.GetFeatureFunctions().begin() ; iterSP != transSystem.GetFeatureFunctions().end() ; ++iterSP) {
    std::string paramShortName = (*iterSP)->GetScoreProducerWeightShortName();
    if (paramShortName == "bl") {
      SetWeight(*iterSP, weight);
      break;
    }
  }
  */
}

// ScoreComponentCollection StaticData::GetAllWeightsScoreComponentCollection() const {}
// in ScoreComponentCollection.h

void StaticData::SetExecPath(const std::string &path)
{
  /*
   namespace fs = boost::filesystem;

   fs::path full_path( fs::initial_path<fs::path>() );

   full_path = fs::system_complete( fs::path( path ) );

   //Without file name
   m_binPath = full_path.parent_path().string();
   */

  // NOT TESTED
  size_t pos = path.rfind("/");
  if (pos !=  string::npos) {
    m_binPath = path.substr(0, pos);
  }
  VERBOSE(1,m_binPath << endl);
}

const string &StaticData::GetBinDirectory() const
{
  return m_binPath;
}

float StaticData::GetWeightWordPenalty() const
{
  float weightWP = GetWeight(&WordPenaltyProducer::Instance());
  //VERBOSE(1, "Read weightWP from translation sytem: " << weightWP << std::endl);
  return weightWP;
}

float StaticData::GetWeightUnknownWordPenalty() const
{
  return GetWeight(&UnknownWordPenaltyProducer::Instance());
}

void StaticData::InitializeForInput(const InputType& source) const
{
  const std::vector<FeatureFunction*> &producers = FeatureFunction::GetFeatureFunctions();
  for(size_t i=0; i<producers.size(); ++i) {
    FeatureFunction &ff = *producers[i];
    if (! IsFeatureFunctionIgnored(ff)) {
      Timer iTime;
      iTime.start();
      ff.InitializeForInput(source);
      VERBOSE(3,"InitializeForInput( " << ff.GetScoreProducerDescription() << " ) = " << iTime << endl);
    }
  }
}

void StaticData::CleanUpAfterSentenceProcessing(const InputType& source) const
{
  const std::vector<FeatureFunction*> &producers = FeatureFunction::GetFeatureFunctions();
  for(size_t i=0; i<producers.size(); ++i) {
    FeatureFunction &ff = *producers[i];
    if (! IsFeatureFunctionIgnored(ff)) {
      ff.CleanUpAfterSentenceProcessing(source);
    }
  }
}

void StaticData::LoadFeatureFunctions()
{
  const std::vector<FeatureFunction*> &ffs
  = FeatureFunction::GetFeatureFunctions();
  std::vector<FeatureFunction*>::const_iterator iter;
  for (iter = ffs.begin(); iter != ffs.end(); ++iter) {
    FeatureFunction *ff = *iter;
    bool doLoad = true;

    // if (PhraseDictionary *ffCast = dynamic_cast<PhraseDictionary*>(ff)) {
    if (dynamic_cast<PhraseDictionary*>(ff)) {
      doLoad = false;
    }

    if (doLoad) {
      VERBOSE(1, "Loading " << ff->GetScoreProducerDescription() << endl);
      ff->Load();
    }
  }

  const std::vector<PhraseDictionary*> &pts = PhraseDictionary::GetColl();
  for (size_t i = 0; i < pts.size(); ++i) {
    PhraseDictionary *pt = pts[i];
    VERBOSE(1, "Loading " << pt->GetScoreProducerDescription() << endl);
    pt->Load();
  }

  CheckLEGACYPT();
}

bool StaticData::CheckWeights() const
{
  set<string> weightNames = m_parameter->GetWeightNames();
  set<string> featureNames;

  const std::vector<FeatureFunction*> &ffs = FeatureFunction::GetFeatureFunctions();
  for (size_t i = 0; i < ffs.size(); ++i) {
    const FeatureFunction &ff = *ffs[i];
    const string &descr = ff.GetScoreProducerDescription();
    featureNames.insert(descr);

    set<string>::iterator iter = weightNames.find(descr);
    if (iter == weightNames.end()) {
      cerr << "Can't find weights for feature function " << descr << endl;
    } else {
      weightNames.erase(iter);
    }
  }

  //sparse features
  if (!weightNames.empty()) {
    set<string>::iterator iter;
    for (iter = weightNames.begin(); iter != weightNames.end(); ) {
      string fname = (*iter).substr(0, (*iter).find("_"));
      VERBOSE(1,fname << "\n");
      if (featureNames.find(fname) != featureNames.end()) {
        weightNames.erase(iter++);
      }
      else {
        ++iter;
      }
    }
  }

  if (!weightNames.empty()) {
    cerr << "The following weights have no feature function. Maybe incorrectly spelt weights: ";
    set<string>::iterator iter;
    for (iter = weightNames.begin(); iter != weightNames.end(); ++iter) {
      cerr << *iter << ",";
    }
    return false;
  }

  return true;
}


void StaticData::LoadSparseWeightsFromConfig() {
  set<string> featureNames;
  const std::vector<FeatureFunction*> &ffs = FeatureFunction::GetFeatureFunctions();
  for (size_t i = 0; i < ffs.size(); ++i) {
    const FeatureFunction &ff = *ffs[i];
    const string &descr = ff.GetScoreProducerDescription();
    featureNames.insert(descr);
  }

  std::map<std::string, std::vector<float> > weights = m_parameter->GetAllWeights();
  std::map<std::string, std::vector<float> >::iterator iter;
  for (iter = weights.begin(); iter != weights.end(); ++iter) {
    // this indicates that it is sparse feature
    if (featureNames.find(iter->first) == featureNames.end()) {
      UTIL_THROW_IF2(iter->second.size() != 1, "ERROR: only one weight per sparse feature allowed: " << iter->first);
        m_allWeights.Assign(iter->first, iter->second[0]);
    }
  }

}


/**! Read in settings for alternative weights */
bool StaticData::LoadAlternateWeightSettings()
{
  if (m_threadCount > 1) {
    cerr << "ERROR: alternative weight settings currently not supported with multi-threading.";
    return false;
  }

  vector<string> weightSpecification;
  const PARAM_VEC *params = m_parameter->GetParam("alternate-weight-setting");
  if (params && params->size()) {
	  weightSpecification = *params;
  }

  // get mapping from feature names to feature functions
  map<string,FeatureFunction*> nameToFF;
  const std::vector<FeatureFunction*> &ffs = FeatureFunction::GetFeatureFunctions();
  for (size_t i = 0; i < ffs.size(); ++i) {
    nameToFF[ ffs[i]->GetScoreProducerDescription() ] = ffs[i];
  }

  // copy main weight setting as default
  m_weightSetting["default"] = new ScoreComponentCollection( m_allWeights );

  // go through specification in config file
  string currentId = "";
  bool hasErrors = false;
  for (size_t i=0; i<weightSpecification.size(); ++i) {

    // identifier line (with optional additional specifications)
    if (weightSpecification[i].find("id=") == 0) {
      vector<string> tokens = Tokenize(weightSpecification[i]);
      vector<string> args = Tokenize(tokens[0], "=");
      currentId = args[1];
      VERBOSE(1,"alternate weight setting " << currentId << endl);
      UTIL_THROW_IF2(m_weightSetting.find(currentId) != m_weightSetting.end(),
    		  "Duplicate alternate weight id: " << currentId);
      m_weightSetting[ currentId ] = new ScoreComponentCollection;

      // other specifications
      for(size_t j=1; j<tokens.size(); j++) {
        vector<string> args = Tokenize(tokens[j], "=");
        // sparse weights
        if (args[0] == "weight-file") {
          if (args.size() != 2) {
            UserMessage::Add("One argument should be supplied for weight-file");
            return false;
          }
          ScoreComponentCollection extraWeights;
          if (!extraWeights.Load(args[1])) {
            UserMessage::Add("Unable to load weights from " + args[1]);
            return false;
          }
          m_weightSetting[ currentId ]->PlusEquals(extraWeights);
        }
        // ignore feature functions
        else if (args[0] == "ignore-ff") {
          set< string > *ffNameSet = new set< string >;
          m_weightSettingIgnoreFF[ currentId ] = *ffNameSet;
          vector<string> featureFunctionName = Tokenize(args[1], ",");
          for(size_t k=0; k<featureFunctionName.size(); k++) {
            // check if a valid nane
            map<string,FeatureFunction*>::iterator ffLookUp
            = nameToFF.find(featureFunctionName[k]);
            if (ffLookUp == nameToFF.end()) {
              cerr << "ERROR: alternate weight setting " << currentId
                   << " specifies to ignore feature function " << featureFunctionName[k]
                   << " but there is no such feature function" << endl;
              hasErrors = true;
            } else {
              m_weightSettingIgnoreFF[ currentId ].insert( featureFunctionName[k] );
            }
          }
        }
      }
    }

    // weight lines
    else {
      UTIL_THROW_IF2(currentId.empty(), "No alternative weights specified");
      vector<string> tokens = Tokenize(weightSpecification[i]);
      UTIL_THROW_IF2(tokens.size() < 2
    		  , "Incorrect format for alternate weights: " << weightSpecification[i]);

      // get name and weight values
      string name = tokens[0];
      name = name.substr(0, name.size() - 1); // remove trailing "="
      vector<float> weights(tokens.size() - 1);
      for (size_t i = 1; i < tokens.size(); ++i) {
        float weight = Scan<float>(tokens[i]);
        weights[i - 1] = weight;
      }

      // check if a valid nane
      map<string,FeatureFunction*>::iterator ffLookUp = nameToFF.find(name);
      if (ffLookUp == nameToFF.end()) {
        cerr << "ERROR: alternate weight setting " << currentId
             << " specifies weight(s) for " << name
             << " but there is no such feature function" << endl;
        hasErrors = true;
      } else {
        m_weightSetting[ currentId ]->Assign( nameToFF[name], weights);
      }
    }
  }
  UTIL_THROW_IF2(hasErrors, "Errors loading alternate weights");
  return true;
}

void StaticData::NoCache()
{
	bool noCache;
	SetBooleanParameter(noCache, "no-cache", false );

	if (noCache) {
		const std::vector<PhraseDictionary*> &pts = PhraseDictionary::GetColl();
		for (size_t i = 0; i < pts.size(); ++i) {
			PhraseDictionary &pt = *pts[i];
			pt.SetParameter("cache-size", "0");
		}
	}
}

std::map<std::string, std::string> StaticData::OverrideFeatureNames()
{
	std::map<std::string, std::string> ret;

	const PARAM_VEC *params = m_parameter->GetParam("feature-name-overwrite");
	if (params && params->size()) {
		UTIL_THROW_IF2(params->size() != 1, "Only provide 1 line in the section [feature-name-overwrite]");
		vector<string> toks = Tokenize(params->at(0));
		UTIL_THROW_IF2(toks.size() % 2 != 0, "Format of -feature-name-overwrite must be [old-name new-name]*");

		for (size_t i = 0; i < toks.size(); i += 2) {
			const string &oldName = toks[i];
			const string &newName = toks[i+1];
			ret[oldName] = newName;
		}
	}

  if (m_useS2TDecoder) {
    // Automatically override PhraseDictionary{Memory,Scope3}.  This will
    // have to change if the FF parameters diverge too much in the future,
    // but for now it makes switching between the old and new decoders much
    // more convenient.
    ret["PhraseDictionaryMemory"] = "RuleTable";
    ret["PhraseDictionaryScope3"] = "RuleTable";
  }

	return ret;
}

void StaticData::OverrideFeatures()
{
  const PARAM_VEC *params = m_parameter->GetParam("feature-overwrite");
  for (size_t i = 0; params && i < params->size(); ++i) {
    const string &str = params->at(i);
    vector<string> toks = Tokenize(str);
    UTIL_THROW_IF2(toks.size() <= 1, "Incorrect format for feature override: " << str);

    FeatureFunction &ff = FeatureFunction::FindFeatureFunction(toks[0]);

    for (size_t j = 1; j < toks.size(); ++j) {
      const string &keyValStr = toks[j];
      vector<string> keyVal = Tokenize(keyValStr, "=");
      UTIL_THROW_IF2(keyVal.size() != 2, "Incorrect format for parameter override: " << keyValStr);

      VERBOSE(1, "Override " << ff.GetScoreProducerDescription() << " "
              << keyVal[0] << "=" << keyVal[1] << endl);

      ff.SetParameter(keyVal[0], keyVal[1]);

    }
  }

}

void StaticData::CheckLEGACYPT()
{
  const std::vector<PhraseDictionary*> &pts = PhraseDictionary::GetColl();
  for (size_t i = 0; i < pts.size(); ++i) {
    const PhraseDictionary *phraseDictionary = pts[i];
    if (dynamic_cast<const PhraseDictionaryTreeAdaptor*>(phraseDictionary) != NULL) {
      m_useLegacyPT = true;
      return;
    }
  }

  m_useLegacyPT = false;
}


void StaticData::ResetWeights(const std::string &denseWeights, const std::string &sparseFile)
{
  m_allWeights = ScoreComponentCollection();

  // dense weights
  string name("");
  vector<float> weights;
  vector<string> toks = Tokenize(denseWeights);
  for (size_t i = 0; i < toks.size(); ++i) {
	const string &tok = toks[i];

	if (tok.substr(tok.size() - 1, 1) == "=") {
	  // start of new feature

	  if (name != "") {
		// save previous ff
		const FeatureFunction &ff = FeatureFunction::FindFeatureFunction(name);
		m_allWeights.Assign(&ff, weights);
		weights.clear();
	  }

	  name = tok.substr(0, tok.size() - 1);
	} else {
	  // a weight for curr ff
	  float weight = Scan<float>(toks[i]);
	  weights.push_back(weight);
	}
  }

  const FeatureFunction &ff = FeatureFunction::FindFeatureFunction(name);
  m_allWeights.Assign(&ff, weights);

  // sparse weights
  InputFileStream sparseStrme(sparseFile);
  string line;
  while (getline(sparseStrme, line)) {
	  vector<string> toks = Tokenize(line);
	  UTIL_THROW_IF2(toks.size() != 2, "Incorrect sparse weight format. Should be FFName_spareseName weight");

	  vector<string> names = Tokenize(toks[0], "_");
	  UTIL_THROW_IF2(names.size() != 2, "Incorrect sparse weight name. Should be FFName_spareseName");

      const FeatureFunction &ff = FeatureFunction::FindFeatureFunction(names[0]);
	  m_allWeights.Assign(&ff, names[1], Scan<float>(toks[1]));
  }
}

} // namespace

