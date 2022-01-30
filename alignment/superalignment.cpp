/***************************************************************************
 *   Copyright (C) 2009 by BUI Quang Minh   *
 *   minh.bui@univie.ac.at   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <stdarg.h>
#include "superalignment.h"
#include "nclextra/msetsblock.h"
#include "nclextra/myreader.h"
#include <model/modelinfo.h>
#include "main/phylotesting.h"
#include "utils/timeutil.h" //for getRealTime()
#include "utils/safe_io.h"  //for safeGetLine()

Alignment *createAlignment(string aln_file, const char *sequence_type,
                           InputType intype, string model_name) {
    bool is_dir = isDirectory(aln_file.c_str());
    if (!is_dir && !contains(aln_file,",")) {
        return new Alignment(aln_file.c_str(),
                             sequence_type, intype, model_name);
    }
    SuperAlignment *super_aln = new SuperAlignment;
    if (is_dir) {
        super_aln->readPartitionDir(aln_file, sequence_type, 
                                    intype, model_name, true);
    }
    else {
        super_aln->readPartitionList(aln_file, sequence_type, 
                                     intype, model_name, true);
    }
    super_aln->init();
    Alignment *aln = super_aln->concatenateAlignments();
    if (aln->isSuperAlignment()) {
        outError("Cannot concatenate alignments"
                 " of different data type ", aln_file);
    }
    delete super_aln;
    return aln;
}

SuperAlignment::SuperAlignment() : Alignment() {
    max_num_states = 0;
}

SuperAlignment::SuperAlignment(Params &params) : Alignment()
{
    readFromParams(params);
    init();

    cout << "Degree of missing data: " 
         << computeMissingData() << endl;
    
#ifdef _OPENMP
    if (params.num_threads > partitions.size()) {
        cout << "Info: multi-threading strategy"
             << " over alignment sites" << endl;
    } else {
        cout << "Info: multi-threading strategy"
             << " over partitions" << endl;
    }
#endif
    cout << endl;

}

void SuperAlignment::readFromParams(Params &params) {
    if (isDirectory(params.partition_file)) {
        // reading all files in the directory
        readPartitionDir(params.partition_file, params.sequence_type, 
                         params.intype, params.model_name, 
                         params.remove_empty_seq);
    } else if (strstr(params.partition_file, ",") != nullptr) {
        // reading all files in a comma-separated list
        readPartitionList(params.partition_file, params.sequence_type, 
                          params.intype, params.model_name, 
                          params.remove_empty_seq);
    } else {
        readPartitionModelFile(params);
    }
    if (partitions.empty()) {
        outError("No partition found");
    }
    
    // check for duplicated partition names
    unordered_set<string> part_names;
    for (auto pit = partitions.begin(); 
         pit != partitions.end(); pit++) {
        if (part_names.find((*pit)->name) != part_names.end())
            outError("Duplicated partition name ", (*pit)->name);
        part_names.insert((*pit)->name);
    }
    
    int subsample = params.subsampling;
    if (subsample != 0) {
        // sumsample a number of partitions
        subSamplePartitions(params, subsample);
    }
    
    writePartitionInfo();
}

void SuperAlignment::readPartitionModelFile(const Params& params) {
    cout << "Reading partition model file " 
         << params.partition_file << " ..." << endl;
    if (detectInputFile(params.partition_file) == InputType::IN_NEXUS) {
        readPartitionNexus(params);
        if (partitions.empty()) {
            outError("No partition found in SETS block." 
                        " An example syntax looks like: \n"
                        "#nexus\n"
                        "begin sets;\n"
                        "  charset part1=1-100;\n" 
                        "  charset part2=101-300;\n"
                        "end;");
        }
    } else {
        readPartitionRaxml(params);
    }
}

void SuperAlignment::subSamplePartitions(Params& params, int subsample) {
    if (abs(subsample) >= partitions.size()) {
        outError("--subsample must be between -" + 
                    convertInt64ToString(partitions.size() - 1) + 
                    " and " + convertInt64ToString(partitions.size() - 1));
    }
    cout << "Random subsampling " 
            << (subsample + ((subsample > 0) ? 0 : partitions.size()))
            << " partitions (seed: " 
            << params.subsampling_seed <<  ")..." << endl;
    int *rstream;
    init_random(params.subsampling_seed, false, &rstream);
    // make sure to sub-sample exact number
    BoolVector sample(partitions.size(), false);
    int i;
    for (int num = 0; num < abs(subsample); ) {
        i = random_int(static_cast<int>(sample.size()), rstream);
        if (!sample[i]) {
            sample[i] = true;
            num++;
        }
    }
    finish_random(rstream);
    if (subsample < 0) {
        // reverse sampling
        for (i = 0; i < sample.size(); i++) {
            sample[i] = !sample[i];
        }
    }
    vector<Alignment*> keep_partitions;
    for (i = 0; i < sample.size(); i++) {
        if (sample[i]) {
            keep_partitions.push_back(partitions[i]);
        }
    }
    // now replace partitions
    partitions = keep_partitions;    
}

void SuperAlignment::writePartitionInfo() const {
    // Initialize the counter for evaluated NNIs on subtrees
    cout << "Subset\tType\tSeqs\tSites\tInfor\tInvar\tModel\tName" << endl;
    int part = 0;
    for (auto it = partitions.begin(); 
         it != partitions.end(); it++, part++) {
        cout << part+1 << "\t" << (*it)->sequence_type
             << "\t" << (*it)->getNSeq()
             << "\t" << (*it)->getNSite() 
             << "\t" << (*it)->num_informative_sites
             << "\t" << (*it)->getNSite()-(*it)->num_variant_sites 
             << "\t" << (*it)->model_name
             << "\t" << (*it)->name << endl;
        if ((*it)->num_variant_sites == 0) {
            outWarning("No variant sites in partition " + (*it)->name);
        } else if ((*it)->num_informative_sites == 0) {
            outWarning("No parsimony-informative sites"
                       " in partition " + (*it)->name);
        }
    }    
}

void SuperAlignment::init(StrVector *sequence_names) {
    // start original code
    
    max_num_states = 0;
	// first build taxa_index and partitions
	size_t nsite = partitions.size();

    // BUG FIX 2016-11-29: when merging partitions 
    // with -m TESTMERGE, sequence order is changed
    // get the taxa names from existing tree
    if (sequence_names && !sequence_names->empty()) {
        seq_names = *sequence_names;
        taxa_index.resize(seq_names.size());
        for (auto i = taxa_index.begin(); 
             i != taxa_index.end(); i++)
            i->resize(nsite, -1);
    }

    size_t site = 0;
	for (auto it = partitions.begin(); 
         it != partitions.end(); ++it, ++site) {
		intptr_t nseq = (*it)->getNSeq();
		//cout << "nseq  = " << nseq << endl;
        seq_to_subset.resize(seq_names.size(),0);
		for (intptr_t seq = 0; seq < nseq; ++seq) {
            auto seq_name = (*it)->getSeqName(seq);
			intptr_t id = getSeqID(seq_name);
			if (id < 0) {
				seq_names.push_back(seq_name);
                seq_to_subset.push_back((*it)->getSequenceSubset(seq));
				id = static_cast<int>(seq_names.size())-1;
				IntVector vec(nsite, -1);
				vec[site] = static_cast<int>(seq);
				taxa_index.push_back(vec);
			} else {
				taxa_index[id][site] = static_cast<int>(seq);
            }
		}
	}
	// now the patterns of sequence-genes presence/absence
	buildPattern();
}

void SuperAlignment::buildPattern() {
	size_t nsite = partitions.size();
	seq_type = SeqType::SEQ_BINARY;
	num_states = 2; // binary type because the super alignment 
                    // presents the presence/absence of taxa
                    // in the partitions
	STATE_UNKNOWN = 2;
	site_pattern.resize(nsite, -1);
	clear();
	pattern_index.clear();
	VerboseMode save_mode = verbose_mode; 
	verbose_mode  = min(verbose_mode, VerboseMode::VB_MIN); 
                    // to avoid printing gappy sites in addPattern
	intptr_t nseq = getNSeq();
	for (size_t site = 0; site < nsite; site++) {
 		Pattern pat;
 		pat.resize(nseq, 0);
        for (intptr_t seq = 0; seq < nseq; seq++) {
            pat[seq] = (taxa_index[seq][site] >= 0) ? 1 : 0;
        }
		addPattern(pat, static_cast<int>(site));
	}
	verbose_mode = save_mode;
	countConstSite();
//    buildSeqStates();
}

void SuperAlignment::readPartition(const Params &params) {
    try {
        ifstream in;
        in.exceptions(ios::failbit | ios::badbit);
        in.open(params.partition_file);
        in.exceptions(ios::badbit);
        
        while (!in.eof()) {
            CharSet info;
            getline(in, info.name, ',');
            if (in.eof()) break;
            getline(in, info.model_name, ',');
            if (model_name == "") info.model_name = params.model_name;
            getline(in, info.aln_file, ',');
            if (info.aln_file == "" && params.aln_file) {
                info.aln_file = params.aln_file;
            }
            getline(in, info.sequence_type, ',');
            if (info.sequence_type=="" && params.sequence_type) {
                info.sequence_type = params.sequence_type;
            }
            safeGetLine(in, info.position_spec);
            trimString(info.sequence_type);
            // cout << endl << "Reading partition " << info.name 
            //      << " (model=" << info.model_name << "," 
            //      << " aln=" << info.aln_file << ","
            //      << " seq=" << info.sequence_type << "," 
            //      << " pos=" << ((info.position_spec.length() >= 20) 
            //                     ? info.position_spec.substr(0,20)+"..." 
            //                     : info.position_spec) 
            //      << ") ..." << endl;

            // TODO move this to supertree
//            info.nniMoves[0].ptnlh = NULL;
//            info.nniMoves[1].ptnlh = NULL;
//            info.cur_ptnlh = NULL;
//            part_info.push_back(info);
            Alignment *part_aln = createAlignment(info.aln_file, 
                                                  info.sequence_type.c_str(),
                                                  params.intype, info.model_name);
            if (!info.position_spec.empty()) {
                Alignment *new_aln = new Alignment();
                new_aln->extractSites(part_aln, info.position_spec.c_str());
                delete part_aln;
                part_aln = new_aln;
            }
            part_aln->name = info.name;
            part_aln->model_name = info.model_name;
            part_aln->position_spec = info.position_spec;
            part_aln->aln_file = info.aln_file;
            part_aln->sequence_type = info.sequence_type;
            partitions.push_back(part_aln);
            // TODO move this to supertree
//            PhyloTree* tree = new PhyloTree(part_aln);
//            push_back(tree);
        }
        
        in.clear();
        // set the failbit again
        in.exceptions(ios::failbit | ios::badbit);
        in.close();
    } catch(ios::failure) {
        outError(ERR_READ_INPUT);
    } catch (string& str) {
        outError(str);
    }
}

void SuperAlignment::readPartitionRaxml(const Params &params) {
    try {
        ifstream in;
        in.exceptions(ios::failbit | ios::badbit);
        in.open(params.partition_file);
        in.exceptions(ios::badbit);
//        PartitionInfo info;
        Alignment *input_aln = NULL;
        if (!params.aln_file) {
            outError("Please supply an alignment with -s option");
        }
        input_aln = createAlignment(params.aln_file, params.sequence_type, 
                                    params.intype, params.model_name);
        
        cout << endl << "Partition file is not in NEXUS format,"
             << " assuming RAxML-style partition file..." << endl;
        
        size_t pos = params.model_name.find_first_of("+*");
        string rate_type = "";
        if (pos != string::npos) {
            rate_type = params.model_name.substr(pos);
        }
        
        while (!in.eof()) {
            std::string model_name;
            getline(in, model_name, ',');
            if (in.eof()) {
                break;
            }
            CharSet info(model_name);
            info.setSequenceTypeAndModelNameFromString(info.model_name);
            info.adjustModelName(rate_type);
            
            getline(in, info.name, '=');
            trimString(info.name);
            if (info.name.empty()) {
                outError("Please give partition names in partition file!");
            }
            safeGetLine(in, info.position_spec);
            trimString(info.position_spec);
            if (info.position_spec.empty()) {
                outError("Please specify alignment positions" 
                         " for partition" + info.name);
            }
            std::replace(info.position_spec.begin(), 
                         info.position_spec.end(), ',', ' ');
            
            // cout << "Reading partition " << info.name 
            //      << " (model=" << info.model_name << ","
            //      << " seq=" << info.sequence_type << "," 
            //      << " pos=" << ((info.position_spec.length() >= 20) 
            //                     ? info.position_spec.substr(0,20)+"..." 
            //                     : info.position_spec) 
            //      << ") ..." << endl;
            
            // TODO to supertree
//            info.nniMoves[0].ptnlh = NULL;
//            info.nniMoves[1].ptnlh = NULL;
//            info.cur_ptnlh = NULL;
//            part_info.push_back(info);
            Alignment *part_aln = new Alignment();
            part_aln->extractSites(input_aln, info.position_spec.c_str());
            
            Alignment *new_aln;
            if (params.remove_empty_seq) {
                new_aln = part_aln->removeGappySeq();
                delete part_aln;
            }
            else {
                new_aln = part_aln;
            }
            new_aln->name          = info.name;
            new_aln->model_name    = info.model_name;
            new_aln->position_spec = info.position_spec;
            new_aln->aln_file      = info.aln_file;
            new_aln->sequence_type = info.sequence_type;
            partitions.push_back(new_aln);
            // TODO move to supertree
            // PhyloTree* tree = new PhyloTree(new_aln);
            // push_back(tree);
            // cout << new_aln->getNSeq() << " sequences"
            //      << " and " << new_aln->getNSite() 
            //      << " sites extracted" << endl;
            // params = origin_params;
        }
        
        in.clear();
        // set the failbit again
        in.exceptions(ios::failbit | ios::badbit);
        in.close();
    } catch(ios::failure) {
        outError(ERR_READ_INPUT);
    } catch (string& str) {
        outError(str);
    }
}

void SuperAlignment::readPartitionNexus(const Params &params) {
//    Params origin_params = params;
    MSetsBlock*   sets_block = new MSetsBlock();
    NxsTaxaBlock* taxa_block = nullptr;
    NxsAssumptionsBlock *assumptions_block = nullptr;
    NxsDataBlock* data_block = nullptr;
    MyReader      nexus(params.partition_file);
    nexus.Add(sets_block);

    if (!params.aln_file) {
        taxa_block        = new NxsTaxaBlock();
        assumptions_block = new NxsAssumptionsBlock(taxa_block);
        data_block        = new NxsDataBlock(taxa_block, assumptions_block);
        nexus.Add(taxa_block);
        nexus.Add(assumptions_block);
        nexus.Add(data_block);
    }

    MyToken token(nexus.inf);
    nexus.Execute(token);
    
    Alignment *input_aln = NULL;
    if (params.aln_file) {
        input_aln = createAlignment(params.aln_file, params.sequence_type, 
                                    params.intype, params.model_name);
    } else {
        if (data_block->GetNTax() > 0) {
            input_aln = new Alignment(data_block, params.sequence_type, 
                                      params.model_name);
        }
        delete data_block;
        delete assumptions_block;
        delete taxa_block;
    }
    
    bool empty_partition = true;
    for (auto it = sets_block->charsets.begin(); 
         it != sets_block->charsets.end(); it++) {
        if ((*it)->model_name != "") {
            empty_partition = false;
            break;
        }
    }
    if (empty_partition) {
        cout << "NOTE: No CharPartition defined, use all CharSets" << endl;
    }
    
    cout << endl << "Loading " << sets_block->charsets.size()
         << " partitions..." << endl;
    
    loadNexusPartitions(params, sets_block, 
                        empty_partition, input_aln);
 
    if (input_aln) {
        delete input_aln;
    }
    delete sets_block;
}

void SuperAlignment::loadNexusPartitions(const Params& params,
                                         MSetsBlock* sets_block,
                                         bool empty_partition,
                                         Alignment* input_aln) {
    for (auto it = sets_block->charsets.begin(); 
         it != sets_block->charsets.end(); it++) {
        if (empty_partition || (*it)->char_partition != "") {
            if ((*it)->model_name == "") {
                (*it)->model_name = params.model_name;
            }
            if ((*it)->aln_file == "" && !input_aln) {
                if (!(*it)->position_spec.empty()) {
                    (*it)->aln_file = (*it)->position_spec;
                    (*it)->position_spec = "";
                } else {
                    outError("No input data for partition ", (*it)->name);
                }
            }
            determineNexusPartitionSequenceType(params, *it);

            // cout << endl << "Reading partition " << info.name 
            //      << " (model=" << info.model_name << ","
            //      << " aln=" << info.aln_file << "," 
            //      << " seq=" << info.sequence_type << ","
            //      << " pos=" << ((info.position_spec.length() >= 20) 
            //                     ? info.position_spec.substr(0,20)+"..." 
            //                     : info.position_spec) 
            //      << ") ..." << endl;
            if ((*it)->sequence_type != "") { 
                auto seq_type= getSeqType((*it)->sequence_type.c_str());
                if (seq_type == SeqType::SEQ_UNKNOWN) {
                    outError("Unknown sequence type " + (*it)->sequence_type);
                }
            }
            loadOneNexusPartition(params, input_aln, *it);
        }
    }   
}

void SuperAlignment::determineNexusPartitionSequenceType
        (const Params& params, CharSet* charset) {
    if (charset->sequence_type=="" && params.sequence_type) {
        charset->sequence_type = params.sequence_type;
    }
    if (charset->sequence_type == "" && !charset->model_name.empty()) {
        // try to get sequence type from model
        //TODO: why compile error?
        auto name = charset->model_name;
        charset->sequence_type = detectSeqTypeName(name.substr(0, 
                                                   name.find_first_of("+*")));
    }
    auto spec = charset->position_spec;
    if (charset->aln_file == "" && ( spec == "" || spec == "*")) {
        outError("Empty position range for partition ", charset->name);
    }
    trimString(charset->sequence_type);    
}

void SuperAlignment::loadOneNexusPartition(const Params& params,
                                           Alignment* input_aln,
                                           CharSet* charset) {
    // TODO move to supertree
    // info.nniMoves[0].ptnlh = NULL;
    // info.nniMoves[1].ptnlh = NULL;
    // info.cur_ptnlh = NULL;
    // part_info.push_back(info);
    Alignment *part_aln;
    if (charset->aln_file != "") {
        part_aln = createAlignment(charset->aln_file, 
                                   charset->sequence_type.c_str(), 
                                   params.intype, charset->model_name);
    } else {
        part_aln = input_aln;
    }
    if (!charset->position_spec.empty() && 
            charset->position_spec != "*") {
        Alignment *new_aln = new Alignment();
        new_aln->extractSites(part_aln, charset->position_spec.c_str());
        if (part_aln != input_aln) delete part_aln;
        part_aln = new_aln;
    }
    if (part_aln->seq_type == SeqType::SEQ_DNA && 
        (charset->sequence_type.substr(0, 5) == "CODON" || 
            charset->sequence_type.substr(0, 5) == "NT2AA")) {
        Alignment *new_aln = new Alignment();
        bool isNT2AA = charset->sequence_type.substr(0, 5) == "NT2AA";
        new_aln->convertToCodonOrAA(part_aln, &charset->sequence_type[5], 
                                    isNT2AA);
        if (part_aln != input_aln) {
            delete part_aln;
        }
        part_aln = new_aln;
    }
    Alignment *new_aln;
    if (params.remove_empty_seq) {
        new_aln = part_aln->removeGappySeq();
    }
    else {
        new_aln = part_aln;
    }
    // also rebuild states set of each sequence 
    // for likelihood computation
    // new_aln->buildSeqStates();
    
    if (part_aln != new_aln && part_aln != input_aln){
        delete part_aln;
    }
    new_aln->name          = charset->name;
    new_aln->model_name    = charset->model_name;
    new_aln->aln_file      = charset->aln_file;
    new_aln->position_spec = charset->position_spec;
    new_aln->sequence_type = charset->sequence_type;
    new_aln->tree_len      = charset->tree_len;
    partitions.push_back(new_aln);
    // PhyloTree* tree = new PhyloTree(new_aln);
    // push_back(tree);
    // params = origin_params;
    // cout << new_aln->getNSeq() << " sequences " 
    //      << "and " << new_aln->getNSite()
    //      << " sites extracted" << endl;
}


void SuperAlignment::readPartitionDir
        (string partition_dir, const char *sequence_type,
         const InputType &intype, const string& model, 
         bool remove_empty_seq) {
    //    Params origin_params = params;
    string dir = partition_dir;
    if (dir.back() != '/') {
        dir.append("/");
    }

    StrVector filenames;
    size_t    file_count = getFilesInDir(partition_dir.c_str(), filenames);
    if (file_count == 0) {
        outError("No file found in ", partition_dir);
    }
    std::sort(filenames.begin(), filenames.end());
    std::cout << "Reading " << file_count << " alignment files"
        << " in directory " << partition_dir << std::endl;
    
    for (auto it = filenames.begin(); it != filenames.end(); it++)
    {
        Alignment *part_aln;
        part_aln = createAlignment(dir+*it, sequence_type, intype, model_name);
//        if (part_aln->seq_type == SeqType::SEQ_DNA && 
//            (strncmp(params.sequence_type, "CODON", 5) == 0 || 
//             strncmp(params.sequence_type, "NT2AA", 5) == 0)) {
//            Alignment *new_aln = new Alignment();
//            bool isNT2AA = strncmp(params.sequence_type, "NT2AA", 5) == 0;
//            new_aln->convertToCodonOrAA(part_aln, params.sequence_type+5, 
//                                        isNT2AA);
//            delete part_aln;
//            part_aln = new_aln;
//        }
        Alignment *new_aln;
        if (remove_empty_seq)
            new_aln = part_aln->removeGappySeq();
        else
            new_aln = part_aln;
        // also rebuild states set of each sequence for likelihood computation
        // new_aln->buildSeqStates();
        
        if (part_aln != new_aln) {
            delete part_aln;
        }
        new_aln->name = *it;
        new_aln->model_name = model_name;
        new_aln->aln_file = dir + *it;
        new_aln->position_spec = "";
        if (sequence_type) {
            new_aln->sequence_type = sequence_type;
        }
        partitions.push_back(new_aln);
    }
}

void SuperAlignment::readPartitionList(string file_list,
                                       const char *sequence_type,
                                       const InputType &intype, 
                                       const string& model,
                                       bool remove_empty_seq)
{
    //    Params origin_params = params;
    
    StrVector    filenames;
    stringstream ss(file_list);
    string       token;
    while (getline(ss, token, ',')) {
        filenames.push_back(token);
    }
    if (filenames.empty()) {
        outError("No file found in ", file_list);
    }
    cout << "Reading " << filenames.size() 
         << " alignment files..." << endl;
    
    for (auto it = filenames.begin(); 
         it != filenames.end(); it++) {
        Alignment *part_aln;
        part_aln = createAlignment(*it, sequence_type, intype, model_name);
        // if (part_aln->seq_type == SeqType::SEQ_DNA && 
        //     (strncmp(params.sequence_type, "CODON", 5) == 0 ||
        //      strncmp(params.sequence_type, "NT2AA", 5) == 0)) {
        //     Alignment *new_aln = new Alignment();
        //     bool isNT2AA = strncmp(params.sequence_type, "NT2AA", 5) == 0
        //     new_aln->convertToCodonOrAA(part_aln, params.sequence_type+5, 
        //                                 isNT2AA);
        //     delete part_aln;
        //     part_aln = new_aln;
        // }
        Alignment *new_aln;
        if (remove_empty_seq) {
            new_aln = part_aln->removeGappySeq();
        }
        else {
            new_aln = part_aln;
        }
        // also rebuild states set of each sequence 
        // for likelihood computation
        // new_aln->buildSeqStates();
        
        if (part_aln != new_aln) {
            delete part_aln;
        }
        new_aln->name = *it;
        new_aln->model_name = model_name;
        new_aln->aln_file = *it;
        new_aln->position_spec = "";
        if (sequence_type) {
            new_aln->sequence_type = sequence_type;
        }
        partitions.push_back(new_aln);
        }
}

void SuperAlignment::printPartition(const char *filename, 
                                    const char *aln_file) const {
    try {
        ofstream out;
        out.exceptions(ios::failbit | ios::badbit);
        out.open(filename);
        printPartition(out, aln_file);
        out.close();
        cout << "Partition information was printed to " << filename << endl;
    } catch (ios::failure &) {
        outError(ERR_WRITE_OUTPUT, filename);
    }
    
}

void SuperAlignment::printPartition(ostream &out, const char *aln_file, 
                                    bool append) const {
    if (append) {
        out << endl;
    }
    else {
        out << "#nexus" << endl;
    }
    if (aln_file) {
        out << "[ partition information for alignment"
            << " written in " << aln_file <<" file ]" << endl;
    }
    out << "begin sets;" << endl;
    int start_site = 1;
    for (size_t part = 0; part < partitions.size(); ++part) {
        string name = partitions[part]->name;
        replace(name.begin(), name.end(), '+', '_');
        int end_site = start_site + partitions[part]->getNSite32();
        out << "  charset " << name
            << " = " << start_site << "-" << end_site-1 << ";" << endl;
        start_site = end_site;
    }
    bool ok_model = true;
    for (size_t part = 0; part < partitions.size(); ++part)
        if (partitions[part]->model_name.empty()) {
            ok_model = false;
            break;
        }
    if (ok_model) {
        out << "  charpartition mymodels =" << endl;
        for (size_t part = 0; part < partitions.size(); ++part) {
            string name = partitions[part]->name;
            replace(name.begin(), name.end(), '+', '_');
            if (part > 0) out << "," << endl;
//            out << "    " << at(part)->getModelNameParams() << ":" << name;
            out << "    " << partitions[part]->model_name 
                << ":" << name;
        }
        out << ";" << endl;
    }
    out << "end;" << endl;
}

void SuperAlignment::printBestPartition(const char *filename) {
    try {
        ofstream out;
        out.exceptions(ios::failbit | ios::badbit);
        out.open(filename);
        out << "#nexus" << endl
        << "begin sets;" << endl;
        int part;
        for (part = 0; part < partitions.size(); part++) {
            string name = partitions[part]->name;
            replace(name.begin(), name.end(), '+', '_');
            out << "  charset " << name << " = ";
            if (!partitions[part]->aln_file.empty()) {
                out << partitions[part]->aln_file << ": ";
            }
            if (partitions[part]->seq_type == SeqType::SEQ_CODON) {
                out << "CODON, ";
            }
            string pos = partitions[part]->position_spec;
            replace(pos.begin(), pos.end(), ',' , ' ');
            out << pos << ";" << endl;
        }
        bool ok_model = true;
        for (part = 0; part < partitions.size(); part++)
            if (partitions[part]->model_name.empty()) {
                ok_model = false;
                break;
            }
        if (ok_model) {
            out << "  charpartition mymodels =" << endl;
            for (part = 0; part < partitions.size(); part++) {
                string name = partitions[part]->name;
                replace(name.begin(), name.end(), '+', '_');
                if (part > 0) {
                    out << "," << endl;
                }
                out << "    " << partitions[part]->model_name << ": " << name;
            }
            out << ";" << endl;
        }
        out << "end;" << endl;
        out.close();
        cout << "Partition information was printed to " << filename << endl;
    } catch (ios::failure &) {
        outError(ERR_WRITE_OUTPUT, filename);
    }
    
}


void SuperAlignment::printPartitionRaxml(const char *filename) {
//    for (part = 0; part < partitions.size(); part++) {
//        if (partitions[part]->aln_file != "") {
//            cout << "INFO: Printing partition in RAxML format"
//                 << " is not possible" << endl;
//            return;
//        }
//    }
    try {
        ofstream out;
        out.exceptions(ios::failbit | ios::badbit);
        out.open(filename);
        int start_site = 1;
        for (int part = 0; part < partitions.size(); part++) {
            string name = partitions[part]->name;
            replace(name.begin(), name.end(), '+', '_');
            int end_site = start_site + partitions[part]->getNSite32();
            switch (partitions[part]->seq_type) {
                case SeqType::SEQ_DNA:    out << "DNA, "; break;
                case SeqType::SEQ_BINARY: out << "BIN, "; break;
                case SeqType::SEQ_MORPH:  out << "MULTI, "; break;
                default: out << partitions[part]->model_name << ","; break;
            }
            out << name << " = " << start_site << "-" << end_site-1 << endl;
            start_site = end_site;
        }
        out.close();
        cout << "Partition information in Raxml format"
             << " was printed to " << filename << endl;
    } catch (ios::failure &) {
        outError(ERR_WRITE_OUTPUT, filename);
    }
    
}

void SuperAlignment::printBestPartitionRaxml(const char *filename) {
//    for (part = 0; part < partitions.size(); part++) {
//        if (partitions[part]->aln_file != "") {
//            cout << "INFO: Printing partition in RAxML format" 
//                 <<" is not possible" << endl;
//            return;
//        }
//    }
    try {
        ofstream out;
        out.exceptions(ios::failbit | ios::badbit);
        out.open(filename);
        for (int part = 0; part < partitions.size(); part++) {
            string part_name = partitions[part]->name;
            replace(part_name.begin(), part_name.end(), '+', '_');
            string model_name = partitions[part]->model_name;
            ModelInfoFromName info(model_name);
            
            
            if (info.hasAscertainmentBiasCorrection()) {
                out << "ASC_";
            }
            switch (partitions[part]->seq_type) {
                case SeqType::SEQ_DNA:    out << "DNA"; break;
                case SeqType::SEQ_BINARY: out << "BIN"; break;
                case SeqType::SEQ_MORPH:  out << "MULTI"; break;
                case SeqType::SEQ_PROTEIN:
                    out << model_name.substr(0, model_name.find_first_of("*{+"));
                    break;
                case SeqType::SEQ_CODON:
                    out << "CODON_" 
                        << model_name.substr(0, model_name.find_first_of("*{+"));
                    break;
                default: out << model_name; break;
            }
            if (contains(model_name,"+FO")) {
                out << "X";
            }
            else if (contains(model_name,"+F")) {
                out << "F";
            }
            out << ", " << name << " = " 
                << partitions[part]->position_spec << endl;
        }
        out.close();
        cout << "Partition information in Raxml format" 
             << " was printed to " << filename << endl;
    } catch (ios::failure &) {
        outError(ERR_WRITE_OUTPUT, filename);
    }
}


void SuperAlignment::linkSubAlignment(int part) {
	ASSERT(taxa_index.size() == getNSeq());
	intptr_t nseq = getNSeq();
	BoolVector checked(partitions[part]->getNSeq(), false);
	for (intptr_t seq = 0; seq < nseq; ++seq) {
		intptr_t id = partitions[part]->getSeqID(getSeqName(seq));
		if (id < 0)
			taxa_index[seq][part] = -1;
		else {
			taxa_index[seq][part] = static_cast<int>(id);
			checked[id] = true;
		}
	}
	// sanity check that all seqnames in partition 
    // must be present in superalignment
	for (size_t seq = 0; seq < checked.size(); seq++) {
		ASSERT(checked[seq]);
	}
}

void SuperAlignment::extractSubAlignment(const Alignment *aln, const IntVector &seq_id, 
                                         int min_true_char, int min_taxa, 
                                         IntVector *kept_partitions) {
	ASSERT(aln->isSuperAlignment());
	const SuperAlignment *saln = dynamic_cast<const SuperAlignment*>(aln);
    name = aln->name;
    model_name = aln->model_name;
    sequence_type = aln->sequence_type;
    position_spec = aln->position_spec;
    aln_file = aln->aln_file;
    seq_to_subset.resize(seq_names.size(),0);
    for (auto it = seq_id.begin(); it != seq_id.end(); it++) {
        ASSERT(*it >= 0 && *it < aln->getNSeq());
        seq_names.push_back(aln->getSeqName(*it));
        seq_to_subset.push_back(aln->getSequenceSubset(*it));
    }

	// BUG HERE!
	//Alignment::extractSubAlignment(aln, seq_id, 0);

	taxa_index.resize(getNSeq());
	for (intptr_t i = 0; i < getNSeq(); ++i) {
		taxa_index[i].resize(saln->partitions.size(), -1);
    }

	int part = 0;
//	partitions.resize(saln->partitions.size());
    partitions.resize(0);
	for (auto ait = saln->partitions.begin(); 
         ait != saln->partitions.end(); ait++, part++) {
		IntVector sub_seq_id;
		for (auto it = seq_id.begin(); it != seq_id.end(); it++) {
			if (saln->taxa_index[*it][part] >= 0) {
				sub_seq_id.push_back(saln->taxa_index[*it][part]);
            }
        }
        if (sub_seq_id.size() < min_taxa) {
            continue;
        }
		Alignment *subaln = new Alignment;
		subaln->extractSubAlignment(*ait, sub_seq_id, 0);
		partitions.push_back(subaln);
		linkSubAlignment(static_cast<int>(partitions.size())-1);
        if (kept_partitions) kept_partitions->push_back(part);
//		cout << subaln->getNSeq() << endl;
//		subaln->printPhylip(cout);
	}

    if (partitions.size() < saln->partitions.size()) {
        for (intptr_t i = 0; i < getNSeq(); ++i) {
            taxa_index[i].resize(partitions.size());
        }
    }

	// now build the patterns based on taxa_index
	buildPattern();
}

SuperAlignment *SuperAlignment::extractPartitions(IntVector &part_id) {
    SuperAlignment *newaln = new SuperAlignment;
    newaln->name = name;
    newaln->model_name = model_name;
    newaln->sequence_type = sequence_type;
    newaln->position_spec = position_spec;
    newaln->aln_file = aln_file;
    newaln->seq_to_subset.resize(newaln->seq_names.size(),0);
    unordered_set<string> seq_names_set;
    IntVector::iterator it;
    for (it = part_id.begin(); it != part_id.end(); ++it) {
        auto seq_begin = partitions[*it]->seq_names.begin();
        auto seq_end   = partitions[*it]->seq_names.end();
        int  seq_no    = 0;
        for (auto seq = seq_begin; seq != seq_end; ++seq, ++seq_no) {
            if (seq_names_set.find(*seq) == seq_names_set.end()) {
                newaln->seq_names.push_back(*seq);
                int subset_no = partitions[*it]->getSequenceSubset(seq_no);
                newaln->seq_to_subset.push_back(subset_no);
                seq_names_set.insert(*seq);
            }
        }
    }
    
    newaln->taxa_index.resize(newaln->getNSeq());
    for (intptr_t i = 0; i < newaln->getNSeq(); ++i) {
        newaln->taxa_index[i].resize(part_id.size(), -1);
    }
    
    size_t part = 0;
    for (auto ait = part_id.begin(); ait != part_id.end(); ++ait, ++part) {
        newaln->partitions.push_back(partitions[*ait]);
        newaln->linkSubAlignment(static_cast<int>(newaln->partitions.size())-1);
    }
    
    // now build the patterns based on taxa_index
    newaln->buildPattern();
    return newaln;
}

void SuperAlignment::removePartitions(set<int> &removed_id) {
    // remove part_id from partitions
    vector<Alignment*> new_partitions;
    int num_partitions = static_cast<int>(partitions.size());
    for (int i = 0; i < num_partitions; ++i)
        if (removed_id.find(i) == removed_id.end()) {
            // not found in the removed set
            new_partitions.push_back(partitions[i]);
        } else {
            delete partitions[i];
            partitions[i] = NULL;
        }
    
    ASSERT(new_partitions.size() + removed_id.size() == partitions.size());
    partitions = new_partitions;

    // get the union seq_names of remaining partitions
    unordered_set<string> seq_names_set;
    seq_names.clear();
    for (auto it = partitions.begin(); 
         it != partitions.end(); it++) {
        auto seq_begin = (*it)->seq_names.begin();
        auto seq_end   = (*it)->seq_names.end();
        for (auto seq = seq_begin; seq != seq_end; ++seq) {
            if (seq_names_set.find(*seq) == seq_names_set.end()) {
                intptr_t seq_no = seq - seq_begin;
                int subset_no = (*it)->getSequenceSubset(seq_no);
                seq_names.push_back(*seq);
                seq_to_subset.push_back(subset_no);
                seq_names_set.insert(*seq);
            }
        }
    }
    
    // build the taxa_index
    taxa_index.resize(getNSeq());

    for (intptr_t i = 0; i < getNSeq(); ++i) {
        taxa_index[i].resize(partitions.size(), -1);
    }
    for (int i = 0; i < num_partitions; ++i) {
        linkSubAlignment(i);
    }
    // now build the patterns based on taxa_index
    buildPattern();
}

std::vector<size_t> SuperAlignment::getSequenceHashes
                        (progress_display_ptr progress) const {
    //JB2020-06-23 Begin : Determine hashes for all the sequences
    double   startHash = getRealTime();
    intptr_t n         = getNSeq();
    vector<size_t> hashes;
    hashes.resize(n, 0);
    #ifdef USE_BOOST
    #ifdef _OPENMP
        #pragma omp parallel for
    #endif
    for (intptr_t seq1=0; seq1<n; ++seq1) {
        size_t hash = 0;
        int part = 0;
        for (auto ait = partitions.begin(); 
             ait != partitions.end(); ait++, part++) {
            int  subseq1 = taxa_index[seq1][part];
            bool present = ( 0 < subseq1 );
            adjustHash(present, hash);
            if (present) {
                for (auto it = (*ait)->begin(); it != (*ait)->end(); it++) {
                    adjustHash((*it)[subseq1],hash);
                }
            }
        }
        hashes[seq1] = hash;
    }
    if (verbose_mode >= VerboseMode::VB_MED) {
        auto hashTime = getRealTime() - startHash;
        cout << "Hashing sequences took " << hashTime 
             << " wall-clock seconds" << endl;
    }
    #endif
    //JB2020-06-23 Finish
    return hashes;
}

Alignment *SuperAlignment::removeIdenticalSeq(string not_remove, bool keep_two, 
                                              StrVector &removed_seqs, 
                                              StrVector &target_seqs) {
    intptr_t nseq = getNSeq();
    BoolVector isSequenceChecked(nseq, false);
    BoolVector isSequenceRemoved(nseq, false);

    #if USE_PROGRESS_DISPLAY
    progress_display progress(nseq*2, isShowingProgressDisabled 
                                      ? "" :  "Checking for duplicate sequences");
    #else
    double progress = 0.0;
    #endif
    
    vector<size_t> hashes = getSequenceHashes(&progress);
    
    bool listIdentical = !Params::getInstance().suppress_duplicate_sequence_warnings;

    auto     startCheck = getRealTime();
	for (intptr_t seq1 = 0; seq1 < nseq; ++seq1, ++progress) {
        if (isSequenceChecked[seq1]) {
            continue;
        }
        bool first_ident_seq = true;
		for (intptr_t seq2 = seq1+1; seq2 < nseq; ++seq2) {
            if (getSeqName(seq2) == not_remove || 
                isSequenceRemoved[seq2]) { 
                continue;
            }
            if (hashes[seq1]!=hashes[seq2]) {
                continue;
            }
			bool equal_seq = areSequencesIdentical(seq1, seq2);
            if (!equal_seq) {
                continue;
            }
            if (static_cast<intptr_t>(removed_seqs.size()) + 3 < getNSeq() && 
                (!keep_two || !first_ident_seq)) {
                removed_seqs.push_back(getSeqName(seq2));
                target_seqs.push_back(getSeqName(seq1));
                isSequenceRemoved[seq2] = true;
            } else {
                reportSequenceKept(seq1, seq2, listIdentical, progress);
            }
            isSequenceChecked[seq2] = true;
            first_ident_seq = false;
		}
		isSequenceChecked[seq1] = true;
	}
    doneCheckingForDuplicateSequences(startCheck, progress);
    if (static_cast<intptr_t>(removed_seqs.size()) + 3 >= getNSeq()) {
        outWarning("Your alignment contains too many identical sequences!");
    }    
    if (removed_seqs.empty()) {
        return this; // do nothing if the list is empty
    }
    return filterOutSequences(isSequenceRemoved);
}

bool SuperAlignment::areSequencesIdentical(intptr_t seq1, intptr_t seq2) {
    int part = 0;
    // check if seq1 and seq2 are identical over all partitions
    for (auto ait = partitions.begin(); 
            ait != partitions.end(); ++ait, part++) {
        int subseq1 = taxa_index[seq1][part];
        int subseq2 = taxa_index[seq2][part];
        if (subseq1 < 0 && subseq2 < 0) {
            // continue if both seqs are absent in this partition
            continue;
        }
        if (subseq1 < 0 && subseq2 > 0) {
            // if one sequence is present and the other is absent 
            // for a gene, we conclude that they are not identical
            return false;
        }
        if (subseq1 > 0 && subseq2 < 0) {
            // if one sequence is present and the other is absent 
            // for a gene, we conclude that they are not identical
            return false;
        }
        // now if both seqs are present, check sequence content
        for (iterator it = (*ait)->begin(); it != (*ait)->end(); ++it) {
            if  ((*it)[subseq1] != (*it)[subseq2]) {
                return false;
            }
        }
    } 
    return true;
}

Alignment* SuperAlignment::filterOutSequences
                (const BoolVector& isSequenceRemoved) {
	// now remove identical sequences
    intptr_t  nseq = getNSeq();
	IntVector keep_seqs;
	for (int seq1 = 0; seq1 < nseq; ++seq1)
    {
        if (!isSequenceRemoved[seq1]) {
            keep_seqs.push_back(seq1);
        }
    }
	SuperAlignment *aln;
	aln = new SuperAlignment;
	aln->extractSubAlignment(this, keep_seqs, 0);
	return aln;
}

int SuperAlignment::checkAbsentStates(const string& msg) {
    int count = 0;
    for (auto it = partitions.begin(); it != partitions.end(); ++it) {
        int64_t     index = it-partitions.begin();
        std::string msg2   = "partition " 
                           + convertInt64ToString((index)+1)
                           + " " + msg;
        count += (*it)->checkAbsentStates(msg2);
    }
    return count;
}

/*
void SuperAlignment::checkGappySeq() {
	int nseq = getNSeq(), part = 0, i;
	IntVector gap_only_seq;
	gap_only_seq.resize(nseq, 1);
	//cout << "Checking gaps..." << endl;
	for (vector<Alignment*>::iterator it = partitions.begin(); 
         it != partitions.end(); it++, part++) {
		IntVector keep_seqs;
		for (i = 0; i < nseq; i++)
			if (taxa_index[i][part] >= 0)
			if (!(*it)->isGapOnlySeq(taxa_index[i][part])) {
				keep_seqs.push_back(taxa_index[i][part]);
				gap_only_seq[i] = 0;
			}
		if (keep_seqs.size() < (*it)->getNSeq()) {
			cout << "Discard " << (*it)->getNSeq() - keep_seqs.size() 
				 << " sequences from partition number " << part+1 << endl;
			Alignment *aln = new Alignment;
			aln->extractSubAlignment((*it), keep_seqs, 0);
			delete (*it);
			(*it) = aln;
			linkSubAlignment(part);
		}
		cout << __func__ << " num_states = " << (*it)->num_states << endl;
	}
	int wrong_seq = 0;
	for (i = 0; i < nseq; i++)
		if (gap_only_seq[i]) {
			cout << "ERROR: Sequence " << getSeqName(i) 
                 << " contains only gaps or missing data" << endl;
			wrong_seq++;
		}
	if (wrong_seq) {
		outError("Some sequences (see above) are problematic,"
                 " please check your alignment again");
		}
}
*/
void SuperAlignment::getSitePatternIndex(IntVector &pattern_index) {
	int nptn = 0;
	for (auto it = partitions.begin(); it != partitions.end(); ++it) {
		int nsite = static_cast<int>(pattern_index.size());
		pattern_index.insert(pattern_index.end(), 
                             (*it)->site_pattern.begin(), 
                             (*it)->site_pattern.end());
		for (int i = nsite; i < pattern_index.size(); i++)
			pattern_index[i] += nptn;
		nptn += static_cast<int>((*it)->getNPattern());
	}
}

void SuperAlignment::getPatternFreq(IntVector &pattern_freq) {
	ASSERT(isSuperAlignment());
	size_t offset = 0;
	if (!pattern_freq.empty()) pattern_freq.resize(0);
	for (auto it = partitions.begin(); it != partitions.end(); ++it) {
		IntVector freq;
		(*it)->getPatternFreq(freq);
		pattern_freq.insert(pattern_freq.end(), freq.begin(), freq.end());
		offset += freq.size();
	}
}

void SuperAlignment::getPatternFreq(int *pattern_freq) {
    ASSERT(isSuperAlignment());
    size_t offset = 0;
    for (auto it = partitions.begin(); 
         it != partitions.end(); ++it) {
        (*it)->getPatternFreq(pattern_freq + offset);
        offset += (*it)->getNPattern();
    }
}

void SuperAlignment::printSiteInfo(const char* filename) {
    try {
        ofstream out(filename);
        printSiteInfoHeader(out, filename, true);
        int id = 1;
        for (auto it = partitions.begin(); 
             it != partitions.end(); ++it, ++id)
            (*it)->printSiteInfo(out, id);
        out.close();
    } catch (...) {
        outError(ERR_WRITE_OUTPUT, filename);
    }
}

void SuperAlignment::computeDivergenceMatrix(double *pair_freq, double *state_freq, 
                                             bool normalize) {
    int nstates = partitions[0]->num_states;
    int square  = nstates*nstates;
    memset(pair_freq, 0, sizeof(double)*square);
    memset(state_freq, 0, sizeof(double)*nstates);
    
    double *part_pair_freq = new double[square];
    double *part_state_freq = new double[nstates];
    
    for (auto it = partitions.begin(); it != partitions.end(); it++) {
        (*it)->computeDivergenceMatrix(part_pair_freq, part_state_freq, false);
        for (int i = 0; i < square; i++) {
            pair_freq[i] += part_pair_freq[i];
        }
        for (int i = 0; i < nstates; i++) {
            state_freq[i] += part_state_freq[i];
        }
    }
    if (normalize) {
        double sum = 0.0;
        for (int i = 0; i < nstates; i++) {
            sum += state_freq[i];
        }
        sum = 1.0/sum;
        for (int i = 0; i < nstates; i++) {
            state_freq[i] *= sum;
        }
        for (int i = 0; i < nstates; i++) {
            sum = 0.0;
            double *pair_freq_ptr = pair_freq + (i*nstates);
            for (int j = 0; j < nstates; j++) {
                sum += pair_freq_ptr[j];
            }
            sum = 1.0/sum;
            for (int j = 0; j < nstates; j++) {
                pair_freq_ptr[j] *= sum;
            }
        }
    }
    delete [] part_state_freq;
    delete [] part_pair_freq;
}

void SuperAlignment::doSymTest(size_t vecid, vector<SymTestResult> &vec_sym, 
                               vector<SymTestResult> &vec_marsym,
                               vector<SymTestResult> &vec_intsym, int *rstream, 
                               vector<SymTestStat> *stats) {
    vector<vector<SymTestStat> >all_stats;
    if (stats)
        all_stats.resize(partitions.size());

    int nparts = static_cast<int>(partitions.size());
    #ifdef _OPENMP
    #pragma omp parallel for
    #endif
    for (int i = 0; i < nparts; i++) {
        if (stats) {
            partitions[i]->doSymTest(vecid + i, vec_sym, vec_marsym, 
                                     vec_intsym, rstream, &all_stats[i]);
            for (auto it = all_stats[i].begin(); it != all_stats[i].end(); ++it) {
                it->part = i;
            }
        } else
            partitions[i]->doSymTest(vecid + i, vec_sym, vec_marsym, 
                                     vec_intsym, rstream);
    }
    if (stats) {
        for (int i = 0; i < nparts; i++)
            stats->insert(stats->end(), all_stats[i].begin(), all_stats[i].end());
    }
}

/*
void SuperAlignment::createBootstrapAlignment(Alignment *aln, 
                                              IntVector* pattern_freq, 
                                              const char *spec) {
	ASSERT(aln->isSuperAlignment());
	Alignment::copyAlignment(aln);
	auto *super_aln = dynamic_cast<SuperAlignment*>(aln);
	ASSERT(partitions.empty());
    name = aln->name;
    model_name = aln->model_name;
    sequence_type = aln->sequence_type;
    position_spec = aln->position_spec;
    aln_file = aln->aln_file;

	if (spec && strncmp(spec, "GENE", 4) == 0) {
		// resampling whole genes
        partitions.resize(super_aln->partitions.size(), NULL);
        int i, ptn;
        for (i = 0; i < super_aln->partitions.size(); i++) {

            // get a random gene
			int part = random_int(super_aln->partitions.size());

            // ptn_freq stores pattern frequency of bootstrap aln

            IntVector ptn_freq;
            if (strncmp(spec,"GENESITE",8) == 0) {
                // resample sites of this gene
                super_aln->partitions[part]->createBootstrapAlignment(ptn_freq);
                ASSERT(ptn_freq.size() == super_aln->partitions[part]->size());
            } else {
                // copy ptn_freq from this gene
                for (ptn = 0; ptn < super_aln->partitions[part]->size(); ptn++)
                    ptn_freq.push_back(super_aln->partitions[part]->at(ptn).frequency);
            }

            if (!partitions[part]) {
                // allocate the partition
                partitions[part] = new Alignment;
                partitions[part]->copyAlignment(super_aln->partitions[part]);
                for (ptn = 0; ptn < super_aln->partitions[part]->size(); ptn++)
                    partitions[part]->at(ptn).frequency = ptn_freq[ptn];
            } else {
                // increase frequency if already existed
                for (ptn = 0; ptn < super_aln->partitions[part]->size(); ptn++)
                    partitions[part]->at(ptn).frequency += ptn_freq[ptn];
            }
        }

        // fulfill genes that are missing
        for (i = 0; i < partitions.size(); i++)
            if (!partitions[i]) {
                partitions[i] = new Alignment;
                partitions[i]->copyAlignment(super_aln->partitions[i]);
                // reset all frequency
                for (ptn = 0; ptn < partitions[i]->size(); ptn++)
                    partitions[i]->at(ptn).frequency = 0;
            }

        // fill up pattern_freq vector
        if (pattern_freq) {
            pattern_freq->resize(0);
            for (i = 0; i < partitions.size(); i++)
                for (ptn = 0; ptn < partitions[i]->size(); ptn++)
                    pattern_freq->push_back(partitions[i]->at(ptn).frequency);
        }
    } else if (!spec) {
		// resampling sites within genes
        for (auto it = super_aln->partitions.begin(); 
             it != super_aln->partitions.end(); ++it) {
            Alignment *boot_aln = new Alignment;
            if (pattern_freq) {
                IntVector part_pattern_freq;
                boot_aln->createBootstrapAlignment(*it, &part_pattern_freq);
                pattern_freq->insert(pattern_freq->end(), 
                                     part_pattern_freq.begin(), 
                                     part_pattern_freq.end());
            } else {
                boot_aln->createBootstrapAlignment(*it);
            }
            partitions.push_back(boot_aln);
        }
    } else {
        outError("Wrong -bsam, either -bsam GENE or -bsam GENESITE");
    }
	taxa_index = super_aln->taxa_index;
    countConstSite();
}
*/

void SuperAlignment::createBootstrapAlignment(Alignment *aln, IntVector* pattern_freq, 
                                              const char *spec) {
    ASSERT(aln->isSuperAlignment());
    SuperAlignment *super_aln = dynamic_cast<SuperAlignment*>(aln);
    ASSERT(partitions.empty());
    name = aln->name;
    model_name = aln->model_name;
    sequence_type = aln->sequence_type;
    position_spec = aln->position_spec;
    aln_file = aln->aln_file;
    
    if (!spec) {
        // resampling sites within genes
        Alignment::copyAlignment(aln);
        partitions.reserve(super_aln->partitions.size());
        for (auto it = super_aln->partitions.begin(); 
             it != super_aln->partitions.end(); ++it) {
            Alignment *boot_aln = new Alignment;
            if (pattern_freq) {
                IntVector part_pattern_freq;
                boot_aln->createBootstrapAlignment(*it, &part_pattern_freq);
                pattern_freq->insert(pattern_freq->end(), 
                                     part_pattern_freq.begin(), 
                                     part_pattern_freq.end());
            } else {
                boot_aln->createBootstrapAlignment(*it);
            }
            partitions.push_back(boot_aln);
        }
        taxa_index = super_aln->taxa_index;
        countConstSite();
    } else if (strcmp(spec, "GENE") == 0) {
        ASSERT(!pattern_freq);
        // resampling whole genes
        IntVector gene_freq;
        random_resampling(static_cast<int>(super_aln->partitions.size()), gene_freq);
        for (int i = 0; i < gene_freq.size(); i++)
            if (gene_freq[i] > 0) {
                Alignment *boot_aln = new Alignment;
                boot_aln->copyAlignment(super_aln->partitions[i]);
                if (gene_freq[i] > 1) {
                    for (auto it = boot_aln->begin(); 
                         it != boot_aln->end(); it++) {
                        it->frequency *= gene_freq[i];
                    }
                    auto site_pattern = boot_aln->site_pattern;
                    for (int j = 1; j < gene_freq[i]; j++)
                        boot_aln->site_pattern.insert(boot_aln->site_pattern.end(), 
                                                      site_pattern.begin(), 
                                                      site_pattern.end());
                    boot_aln->countConstSite();
                }
                partitions.push_back(boot_aln);
            }
        init();
    } else if (strcmp(spec, "GENESITE") == 0) {
        ASSERT(!pattern_freq);
        // resampling whole genes then sites within resampled genes
        IntVector gene_freq;
        random_resampling(static_cast<int>(super_aln->partitions.size()), gene_freq);
        for (int i = 0; i < gene_freq.size(); i++)
            for (int rep = 0; rep < gene_freq[i]; rep++) {
            Alignment *boot_aln = new Alignment;
            boot_aln->createBootstrapAlignment(super_aln->partitions[i]);
            boot_aln->name = boot_aln->name + "." + convertIntToString(rep);
            partitions.push_back(boot_aln);
        }
        init();
    } else {
        outError("Wrong -bsam, either -bsam GENE or -bsam GENESITE");
    }
}

void SuperAlignment::createBootstrapAlignment(IntVector &pattern_freq, 
                                              const char *spec) {
	ASSERT(isSuperAlignment());
	int nptn = 0;
	for (auto it = partitions.begin(); 
         it != partitions.end(); ++it) {
		nptn += static_cast<int>((*it)->getNPattern());
	}
	pattern_freq.resize(0);
	int *internal_freq = new int[nptn];
	createBootstrapAlignment(internal_freq, spec);
	pattern_freq.insert(pattern_freq.end(), internal_freq, 
                        internal_freq + nptn);
	delete [] internal_freq;

}


void SuperAlignment::createBootstrapAlignment(int *pattern_freq, 
                                              const char *spec, int *rstream) {
	ASSERT(isSuperAlignment());
    // if (spec && strncmp(spec, "GENE", 4) != 0) {
    //     outError("Unsupported yet. ", __func__);
    // }
	if (spec && strncmp(spec, "GENE", 4) == 0) {
		// resampling whole genes
		intptr_t nptn = 0;
		IntVector part_pos;
		for (auto it = partitions.begin(); 
             it != partitions.end(); ++it) {
			part_pos.push_back(static_cast<int>(nptn));
			nptn += (*it)->getNPattern();
		}
        for (int i=0; i<nptn; ++i) {
            pattern_freq[i] = 0;
        }
        IntVector gene_freq;
        random_resampling(static_cast<int>(partitions.size()), 
                          gene_freq, rstream);
		for (int part = 0; part < partitions.size(); part++)
        for (int rep = 0; rep < gene_freq[part]; rep++){
			Alignment *aln = partitions[part];
			if (strncmp(spec,"GENESITE",8) == 0) {
				// then resampling sites in resampled gene
                IntVector sample;
                random_resampling(static_cast<int>(aln->getNSite()), 
                                  sample, rstream);
				for (int site = 0; site < sample.size(); site++)
                for (int rep2 = 0; rep2 < sample[site]; rep2++) {
					int ptn_id = aln->getPatternID(site);
					pattern_freq[ptn_id + part_pos[part]]++;
				}
			} else {
				for (int ptn = 0; ptn < aln->getNPattern(); ptn++) {
					pattern_freq[ptn + part_pos[part]] += aln->at(ptn).frequency;
                }
			}
		}
	} else {
		// resampling sites within genes
		intptr_t offset = 0;
		for (auto it = partitions.begin(); it != partitions.end(); it++) {
            if (spec && strncmp(spec, "SCALE=", 6) == 0) {
                (*it)->createBootstrapAlignment(pattern_freq + offset, 
                                                spec, rstream);
            }
            else {
                (*it)->createBootstrapAlignment(pattern_freq + offset, 
                                                nullptr, rstream);
            }
			offset += (*it)->getNPattern();
		}
	}
}

/**
 * shuffle alignment by randomizing the order of sites
 */
void SuperAlignment::shuffleAlignment() {
	ASSERT(isSuperAlignment());
	for (auto it = partitions.begin(); 
         it != partitions.end(); it++) {
		(*it)->shuffleAlignment();
	}
}


double SuperAlignment::computeObsDist(int seq1, int seq2) const {
	int diff_pos = 0, total_pos = 0;
	for (size_t site = 0; site < getNSite(); ++site) {
		int id1 = taxa_index[seq1][site];
		int id2 = taxa_index[seq2][site];
		if (id1 < 0 || id2 < 0) continue;
		int num_states = partitions[site]->num_states;
		for (auto it = partitions[site]->begin(); 
             it != partitions[site]->end(); it++) 
			if  (static_cast<int>((*it)[id1]) < num_states && 
                 static_cast<int>((*it)[id2]) < num_states) {
				total_pos += (*it).frequency;
				if ((*it)[id1] != (*it)[id2] )
					diff_pos += (*it).frequency;
			}
	}
	if (!total_pos)
		return MAX_GENETIC_DIST; // return +INF if no overlap between two sequences
	return ((double)diff_pos) / total_pos;
}


double SuperAlignment::computeDist(int seq1, int seq2) const {
	if (partitions.empty()) return 0.0;
	double obs_dist = computeObsDist(seq1, seq2);
    int num_states = partitions[0]->num_states;
    double z = (double)num_states / (num_states-1);
    double x = 1.0 - (z * obs_dist);

    if (x <= 0) {
        // string str = "Too long distance between two sequences ";
        // str += getSeqName(seq1);
        // str += " and ";
        // str += getSeqName(seq2);
        // outWarning(str);
        return MAX_GENETIC_DIST;
    }

    return -log(x) / z;
    //return computeObsDist(seq1, seq2);
	//  AVERAGE DISTANCE

	double dist = 0;
	int part = 0, num = 0;
	for (auto it = partitions.begin(); 
         it != partitions.end(); ++it, ++part) {
		int id1 = taxa_index[seq1][part];
		int id2 = taxa_index[seq2][part];
		if (id1 < 0 || id2 < 0) {
            continue;
        }
		dist += (*it)->computeDist(id1, id2);
        ++num;
	}
	if (num == 0) { // two sequences are not overlapping at all!
		return MAX_GENETIC_DIST;
    }
	return dist / num;
}

SuperAlignment::~SuperAlignment()
{
	for (auto it = partitions.rbegin(); 
         it != partitions.rend(); ++it) {
		delete (*it);
    }
	partitions.clear();
}

void SuperAlignment::printAlignment(InputType format, ostream &out, 
                                    const char* file_name, bool append, 
                                    const char *aln_site_list,
                                    int exclude_sites, const char *ref_seq_name,
                                    bool report_progress) const
{
    //Ask for all of them
    set<int> all_the_partition_ids;
    for (int i=0; i<partitions.size(); ++i) {
        all_the_partition_ids.insert(i);
    }
    Alignment *concat = concatenateAlignments(all_the_partition_ids);
    ASSERT(!concat->isSuperAlignment());
    concat->Alignment::printAlignment(format, out, file_name, append, aln_site_list,
                                      exclude_sites, ref_seq_name, report_progress);
    delete concat;
    if (format == InputType::IN_NEXUS) {
        printPartition(out, NULL, true);
    }
}

void SuperAlignment::printSubAlignments(Params &params) {
	vector<Alignment*>::iterator pit;
	string filename;
	int part;
	for (pit = partitions.begin(), part = 0; 
         pit != partitions.end(); ++pit, ++part) {
		if (params.aln_output) {
			filename = params.aln_output;
        }
		else {
			filename = params.out_prefix;
        }
		filename += "." + (*pit)->name;
        int exclude_sites = (params.aln_nogaps) ? EXCLUDE_GAP : 0;
        (*pit)->printAlignment(params.aln_output_format, filename.c_str(), 
                               false, nullptr, exclude_sites, nullptr);
	}
}

double SuperAlignment::computeUnconstrainedLogL() const {
	double logl = 0.0;
	for (auto pit = partitions.begin(); 
         pit != partitions.end(); ++pit) {
		logl += (*pit)->computeUnconstrainedLogL();
    }
	return logl;
}

double SuperAlignment::computeMissingData() {
	double ret = 0.0;
	size_t len = 0;
	for (auto pit = partitions.begin();
         pit != partitions.end(); ++pit) {
		ret += (*pit)->getNSeq() * (*pit)->getNSite();
		len += (*pit)->getNSite();
	}
	ret /= getNSeq() * len;
	return 1.0 - ret;
}

void SuperAlignment::identifyUnionTaxa
        (const std::set<int>& ids, std::string& union_taxa, 
         int& nsites, int& nstates, SeqType& sub_type ) const {
	for (auto it = ids.begin(); it != ids.end(); ++it) {
		int id = *it;
		ASSERT(id >= 0 && id < partitions.size());
		if (nstates == 0) {
            nstates = partitions[id]->num_states;
        }
		if (sub_type == SeqType::SEQ_UNKNOWN) {
            sub_type = partitions[id]->seq_type;
        }
		if (sub_type != partitions[id]->seq_type) {
			outError("Cannot concatenate sub-alignments of different type");
        }
		if (nstates != partitions[id]->num_states) {
			outError("Cannot concatenate sub-alignments of different #states");
        }
		string taxa_set;
        Pattern taxa_pat = getPattern(id);
        taxa_set.insert(taxa_set.begin(), taxa_pat.begin(), taxa_pat.end());
		nsites += static_cast<int>(partitions[id]->getNSite());
		if (it == ids.begin()) {
            union_taxa = taxa_set; 
        } else {
			for (int j = 0; j < union_taxa.length(); ++j) {
				if (taxa_set[j] == 1) { 
                    union_taxa[j] = 1;
                }
            }
		}
	}
}

Alignment *SuperAlignment::concatenateAlignments(set<int> &ids) const {
	string  union_taxa;
	int     nsites   = 0;
    int     nstates  = 0;
	SeqType sub_type = SeqType::SEQ_UNKNOWN;

    identifyUnionTaxa(ids, union_taxa, nsites, nstates, sub_type);

	Alignment *aln = new Alignment;
	for (int i = 0; i < union_taxa.length(); i++) {
		if (union_taxa[i] == 1) {
			aln->seq_names.push_back(getSeqName(i));
            aln->seq_to_subset.push_back(getSequenceSubset(i));
		}
    }
	aln->num_states = nstates;
	aln->seq_type   = sub_type;
	aln->site_pattern.resize(nsites, -1);
    aln->clear();
    aln->pattern_index.clear();
    aln->STATE_UNKNOWN  = partitions[*ids.begin()]->STATE_UNKNOWN;
    aln->genetic_code   = partitions[*ids.begin()]->genetic_code;
    aln->codon_table    = partitions[*ids.begin()]->codon_table;
    aln->non_stop_codon = partitions[*ids.begin()]->non_stop_codon;

    int site = 0;
    for (auto itId = ids.begin(); itId != ids.end(); ++itId) {
    	int id = *itId;
        // 2018-08-23: important bugfix in v1.6: taxa_set has wrong correspondance
		//string taxa_set;
        //Pattern taxa_pat = getPattern(id);
        //taxa_set.insert(taxa_set.begin(), taxa_pat.begin(), taxa_pat.end());
    	for (Alignment::iterator it = partitions[id]->begin(); 
             it != partitions[id]->end(); ++it) {
    		Pattern pat;
    		//int part_seq = 0;
            intptr_t union_seq_count = union_taxa.size();
    		for (intptr_t seq = 0; seq < union_seq_count; ++seq)
    			if (union_taxa[seq] == 1) {
    				char ch = aln->STATE_UNKNOWN;
                    int seq_part = taxa_index[seq][id];
                    if (seq_part >= 0) {
                        ch = (*it)[seq_part];
                    }
                    //if (taxa_set[seq] == 1) {
                    //    ch = (*it)[part_seq++];
                    //}
    				pat.push_back(ch);
    			}
    		//ASSERT(part_seq == partitions[id]->getNSeq());
    		aln->addPattern(pat, site, (*it).frequency);
    		// IMPORTANT BUG FIX FOLLOW
    		int ptnindex = aln->pattern_index[pat];
            for (int j = 0; j < (*it).frequency; ++j) {
                aln->site_pattern[site++] = ptnindex;
            }
    	}
    }
    aln->countConstSite();
//    aln->buildSeqStates();
	return aln;
}

Alignment *SuperAlignment::concatenateAlignments() const {
    vector<SeqType>   seq_types;
    StrVector         genetic_codes;
    vector<set<int> > ids;
    for (int i = 0; i < partitions.size(); ++i) {
        bool found = false;
        for (int j = 0; j < seq_types.size(); ++j)
            if (partitions[i]->seq_type == seq_types[j] &&
                partitions[i]->genetic_code == genetic_codes[j]) {
                ids[j].insert(i);
                found = true;
                break;
            }
        if (found) {
            continue;
        }
        // create a new partition
        seq_types.push_back(partitions[i]->seq_type);
        genetic_codes.push_back(partitions[i]->genetic_code);
        ids.push_back(set<int>());
        ids.back().insert(i);
    }
    if (seq_types.size() == 1) {
        return concatenateAlignments(ids[0]);
    }

    // mixed data with >= 2 partitions
    SuperAlignment *saln = new SuperAlignment();
    saln->max_num_states = 0;
    // first build taxa_index and partitions
    size_t nsite = ids.size();
    
    // BUG FIX 2016-11-29: when merging partitions 
    // with -m TESTMERGE, sequence order is changed
    // get the taxa names from existing tree
    
    saln->seq_names = seq_names;
    saln->taxa_index.resize(saln->seq_names.size());
    for (auto it = saln->taxa_index.begin();
         it != saln->taxa_index.end(); ++it) {
        it->resize(nsite, -1);
    }
    for (size_t site = 0; site != nsite; ++site) {
        Alignment *part_aln = concatenateAlignments(ids[site]);
        saln->partitions.push_back(part_aln);
        intptr_t nseq = part_aln->getNSeq();
        //cout << "nseq  = " << nseq << endl;
        for (intptr_t seq = 0; seq < nseq; ++seq) {
            intptr_t id = saln->getSeqID(part_aln->getSeqName(seq));
            ASSERT(id >= 0);
            saln->taxa_index[id][site] = static_cast<int>(seq);
        }
    }
    // now the patterns of sequence-genes presence/absence
    saln->buildPattern();
    return saln;
}

void SuperAlignment::countConstSite() {
    num_informative_sites = 0;
    num_variant_sites     = 0;
    max_num_states        = 0;
    frac_const_sites      = 0;
    frac_invariant_sites  = 0;
    num_parsimony_sites   = 0;
    size_t nsites         = 0;
    for (auto it = partitions.begin(); 
         it != partitions.end(); ++it) {
        (*it)->countConstSite();
        num_informative_sites += (*it)->num_informative_sites;
        num_variant_sites     += (*it)->num_variant_sites;
        if ((*it)->num_states > max_num_states) {
            max_num_states = (*it)->num_states;
        }
        nsites                += (*it)->getNSite();
        frac_const_sites      += (*it)->frac_const_sites * (*it)->getNSite();
        frac_invariant_sites  += (*it)->frac_invariant_sites * (*it)->getNSite();
    }
    frac_const_sites /= nsites;
    frac_invariant_sites /= nsites;
}

void SuperAlignment::orderPatternByNumChars(int pat_type) {
    const int UINT_BITS = sizeof(UINT)*8;
    if (pat_type == PAT_INFORMATIVE) {
        num_parsimony_sites = num_informative_sites;
    }
    else {
        num_parsimony_sites = num_variant_sites;
    }
    int maxi = (num_parsimony_sites+UINT_BITS-1)/UINT_BITS;
    delete [] pars_lower_bound;
    pars_lower_bound = nullptr;
    pars_lower_bound = new UINT[maxi+1];
    memset(pars_lower_bound, 0, (maxi+1)*sizeof(UINT));
    size_t nseq = getNSeq();
    
    // compute ordered_pattern
    ordered_pattern.clear();
//    UINT sum_scores[npart];
    for (size_t part  = 0; part != partitions.size(); ++part) {
        partitions[part]->orderPatternByNumChars(pat_type);
        // partial_partition
        if (Params::getInstance().partition_type == TOPO_UNLINKED) {
            continue;
        }
        for (auto pit = partitions[part]->ordered_pattern.begin();
             pit != partitions[part]->ordered_pattern.end(); ++pit) {
            Pattern pattern(*pit);
            pattern.resize(nseq); // maximal unknown states
            for (int j = 0; j < nseq; ++j) {
                if (taxa_index[j][part] >= 0) {
                    pattern[j] = (*pit)[taxa_index[j][part]];
                }
                else {
                    pattern[j] = partitions[part]->STATE_UNKNOWN;
                }
            }
            ordered_pattern.push_back(pattern);
        }
//        sum_scores[part] = partitions[part]->pars_lower_bound[0];
    }
    // TODO compute pars_lower_bound (lower bound 
    // of pars score for remaining patterns)
}
