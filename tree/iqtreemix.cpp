//
//  iqtreemix.cpp
//  tree
//
//  Created by Thomas Wong on 14/12/20.
//

#include "iqtreemix.h"
const double MIN_PROP = 0.001;
const double MAX_PROP = 1000.0;

// Input formats for the tree-mixture model
// 1. linked models and site rates: GTR+G4+T2
// 2. unlinked models and linked site rates: MIX{GTR,GTR}+G4+T2
// 3. linked models and unlinked site rates: GTR+MIX{G4,E}+T2
// 4. unlinked models and unlinked site rates: MIX{GTR+G4,GTR}+T2
// The situation that a part of the model is linked while another part is unlinked is not allowed.
//    For example, MIX{GTR,GTR}+FO+T2 or GTR+MIX{FO+F0}+T2 is not be accepted
// Similarly, the situation that a part of the site rate is linked while another part is unlinked is also not allowed.
//    For example, GTR+MIX{I,I}+G4+T2 or GTR+I+MIX{G4+G4}+T2 is not be accepted

IQTreeMix::IQTreeMix() : IQTree() {
    patn_freqs = NULL;
    patn_isconst = NULL;
    ptn_like_cat = NULL;
    _ptn_like_cat = NULL;
}

IQTreeMix::IQTreeMix(Params &params, Alignment *aln, vector<IQTree*> &trees) : IQTree(aln) {
    size_t i;

    clear();
    weights.clear();

    // store the trees and initialize tree-weights
    double init_weight = 1.0 / (double) trees.size();
    for (i=0; i<trees.size(); i++) {
        push_back(trees[i]);
        weights.push_back(init_weight);
    }
    
    // allocate memory for the arrays
    ptn_like_cat = new double[size() * aln->getNPattern()];
    _ptn_like_cat = new double[size() * aln->getNPattern()];
    patn_freqs = new int[aln->getNPattern()];
    patn_isconst = new int[aln->getNPattern()];
    
    // get the pattern frequencies
    aln->getPatternFreq(patn_freqs);
    
    // get whether the pattern is constant
    for (i=0; i<aln->getNPattern(); i++) {
        patn_isconst[i] = aln->at(i).isConst();
    }
    
    // number of optimization steps, default: number of Trees * 2
    // optimize_steps = 2 * size();
    optimize_steps = 100;
}

IQTreeMix::~IQTreeMix() {
    size_t i;
    
    // break the linkages
    if (models.size() == 1) {
        // share both model and site rate
        for (i=1; i<size(); i++) {
            at(i)->setModelFactory(NULL);
            at(i)->setModel(NULL);
            at(i)->setRate(NULL);
        }
    } else if (site_rates.size() == 1) {
        // only share site rate
        for (i=1; i<size(); i++) {
            at(i)->getModelFactory()->site_rate = NULL;
            at(i)->setRate(NULL);
        }
    }
    for (i=0; i<size(); i++) {
        at(i)->setParams(NULL);
    }
    setModelFactory(NULL);
    setModel(NULL);
    setRate(NULL);

    for (i=0; i<size(); i++) {
        delete (at(i));
    }
    if (ptn_like_cat != NULL) {
        delete[] ptn_like_cat;
    }
    if (_ptn_like_cat != NULL) {
        delete[] _ptn_like_cat;
    }
    if (patn_freqs != NULL) {
        delete[] patn_freqs;
    }
    if (patn_isconst != NULL) {
        delete[] patn_isconst;
    }
}

void separateStr(string str, vector<string>& substrs, char separator) {
    int startpos = 0;
    int pos = 0;
    int brac_num = 0;
    while (pos < str.length()) {
        if (str[pos] == '{')
            brac_num++;
        else if (str[pos] == '}')
            brac_num--;
        else if (str[pos] == separator && brac_num<=0) {
            if (pos - startpos > 0)
                substrs.push_back(str.substr(startpos, pos-startpos));
            brac_num = 0;
            startpos = pos+1;
        }
        pos++;
    }
    if (pos - startpos > 0)
        substrs.push_back(str.substr(startpos, pos-startpos));
}

void divideModelNSiteRate(string name, string& model, string& siteRate) {
    string orig, s;
    size_t plus_pos;
    int i;
    // assume model always exists
    
    orig = name;
    model = "";
    siteRate = "";
    i = 0;
    while (name.length() > 0) {
        plus_pos = name.find("+");
        if (plus_pos != string::npos) {
            s = name.substr(0,plus_pos);
            name = name.substr(plus_pos+1);
        } else {
            s = name;
            name = "";
        }
        if (s.length() == 0) {
            outError(orig + " is not a valid model");
        }
        if (i==0 || (s[0]=='F')) {
            if (model.length() > 0)
                model.append("+");
            model.append(s);
        } else {
            if (siteRate.length() > 0)
                siteRate.append("+");
            siteRate.append(s);
        }
        i++;
    }
}

void rmSpace(string& s) {
    size_t i,k;
    k=0;
    for (i=0; i<s.length(); i++) {
        if (s[i]!=' ') {
            if (k < i) {
                s[k] = s[i];
            }
            k++;
        }
    }
    if (k == 0)
        s = "";
    else if (k < s.length())
        s = s.substr(0,k);
}

// to separate the submodel names and the site rate names from the full model name
void IQTreeMix::separateModel(string modelName) {
    size_t t_pos, i, k;
    string s;
    vector<string> model_array, submodel_array;
    
    rmSpace(modelName);
    treemix_model = modelName;
    model_names.clear();
    siterate_names.clear();
    isLinkSiteRate = true; // initialize to true
    
    // check how many trees
    cout << "[IQTreeMix::separateModel] modelName = " << modelName << endl;
    t_pos = modelName.rfind("+T");
    if (t_pos == string::npos) {
        outError("This model is not a tree mixture model, because there is no '+T'");
    }
    if (t_pos >= modelName.length()-2) {
        outError("You need to specific the number of trees after '+T', e.g. +T2 for 2 trees");
    }
    ntree = atoi(modelName.substr(t_pos+2).c_str());
    /*
    if (ntree <= 1) {
        outError("For tree mixture model, number of trees has to be at least 2.");
    }*/
    
    // remove the '+Txx'
    modelName = modelName.substr(0, t_pos);
    
    // break the whole name according to '+'
    separateStr(modelName, model_array, '+');
    
    // check each model/siterate
    for (i=0; i<model_array.size(); i++) {
        s = model_array[i];
        if (s.length() == 0) {
            continue;
        } else if (s.length() > 5 && s.substr(0,4) == "MIX{" && s.substr(s.length()-1,1) == "}") {
            // mixture model
            s = s.substr(4,s.length()-5); // remove the beginning "MIX{" and the ending "}"
            if (i==0) {
                // unlinked models (while site rates may or may not be linked)
                bool siteRateAppear = false;
                string curr_model, curr_siterate;
                separateStr(s, submodel_array, ',');
                for (k=0; k<submodel_array.size(); k++) {
                    divideModelNSiteRate(submodel_array[k], curr_model, curr_siterate);
                    model_names.push_back(curr_model);
                    siterate_names.push_back(curr_siterate);
                    if (curr_siterate.length() > 0) {
                        siteRateAppear = true; // some site rates appear
                    }
                }
                if (!siteRateAppear) {
                    // all siterate_names are empty, thus remove them
                    siterate_names.clear();
                }
            } else if (siterate_names.size()==0) {
                // unlinked site rates
                separateStr(s, submodel_array, ',');
                for (k=0; k<submodel_array.size(); k++) {
                    siterate_names.push_back(submodel_array[k]);
                }
            } else {
                outError("Error! The model: " + treemix_model + " is not correctly specified. Are you using too many 'MIX'?");
            }
        } else if (i==0) {
            // not a mixture model
            model_names.push_back(s);
        } else if (s.length() <= 2 && s[0] == 'F') {
            // F or FO
            if (model_names.size() > 1) {
                outError("Error! 'F' is linked, but the model is unlinked");
            } else if (model_names.size() == 1){
                model_names[0].append("+" + s);
            } else {
                outError("Error! 'F' appears before the model does");
            }
        } else {
            // assume this is the site rate model
            if (siterate_names.size() > 1) {
                outError("Error! '" + s + "' is linked, but the site rates are unlinked");
            } else if (siterate_names.size() == 1) {
                siterate_names[0].append("+" + s);
            } else {
                siterate_names.push_back(s);
            }
        }
    }
    if (model_names.size() == 0) {
        outError("Error! It seems no model is defined.");
    }
    isLinkModel = (model_names.size() == 1);
    if (siterate_names.size() == 0) {
        anySiteRate = false;
    } else {
        anySiteRate = true;
        isLinkSiteRate = (siterate_names.size() == 1);
    }
    
    // check correctness
    if (model_names.size() > 1 && model_names.size() != ntree) {
        outError("Error! The number of submodels specified in the mixture does not match with the tree number");
    }
    if (siterate_names.size() > 1 && siterate_names.size() != ntree) {
        outError("Error! The number of site rates specified in the mixture does not match with the tree number");
    }
}

void IQTreeMix::initializeModel(Params &params, string model_name, ModelsBlock *models_block) {
    size_t i;
    string curr_model;

    models.clear();
    site_rates.clear();
    separateModel(model_name);
    
    // initialize the models
    for (i=0; i<ntree; i++) {
        if (isLinkModel)
            curr_model = model_names[0];
        else
            curr_model = model_names[i];
        if (anySiteRate) {
            if (isLinkSiteRate)
                curr_model += "+" + siterate_names[0];
            else
                curr_model += "+" + siterate_names[i];
        }
        cout << "model: " << curr_model << endl;
        at(i)->initializeModel(params, curr_model, models_block);
        if (i==0) {
            // also initialize the model for this tree
            IQTree::initializeModel(params, curr_model, models_block);
        }
    }
    
    // handle the linked or unlinked substitution model(s)
    if (isLinkModel) {
        models.push_back(at(0)->getModelFactory()->model);
        for (i=1; i<ntree; i++) {
            ModelSubst *m = at(i)->getModelFactory()->model;
            // delete m (need to be addressed);
            at(i)->getModelFactory()->model = models[0];
            at(i)->setModel(models[0]);
        }
    } else {
        for (i=0; i<ntree; i++) {
            models.push_back(at(i)->getModelFactory()->model);
        }
    }
    
    // handle the linked or unlinked site rate(s)
    if (anySiteRate) {
        if (isLinkSiteRate) {
            site_rates.push_back(at(0)->getModelFactory()->site_rate);
            for (i=1; i<ntree; i++) {
                RateHeterogeneity *r = at(i)->getModelFactory()->site_rate;
                // delete r (need to be addressed);
                at(i)->getModelFactory()->site_rate = site_rates[0];
                at(i)->setRate(site_rates[0]);
            }
        } else {
            for (i=0; i<ntree; i++) {
                site_rates.push_back(at(i)->getModelFactory()->site_rate);
            }
        }
    }
    
    /*
    // set the trees of the rate models to this tree
    for (i=0; i<models.size(); i++) {
        if (models[i]) {
            ((ModelMarkov*)models[i])->setTree(this);
        }
    }
*/
    // set the trees of the site rates to this tree
    for (i=0; i<site_rates.size(); i++) {
        if (site_rates[i]) {
            site_rates[i]->setTree(this);
        }
    }
}

double IQTreeMix::computeLikelihood(double *pattern_lh) {
    double* pattern_lh_tree;
    size_t i,j,ptn,t;
    size_t nptn,ntree;
    double logLike = 0.0;
    double subLike;
    double score;
    PhyloTree* ptree;
    
    nptn = aln->getNPattern();
    ntree = size();

    // compute likelihood for each tree
    pattern_lh_tree = _ptn_like_cat;
    for (t=0; t<ntree; t++) {
        // save the site rate's tree
        ptree = at(t)->getRate()->getTree();
        // set the tree t as the site rate's tree
        // and compute the likelihood values
        at(t)->getRate()->setTree(at(t));
        at(t)->initializeAllPartialLh();
        score = at(t)->computeLikelihood(pattern_lh_tree);
        at(t)->clearAllPartialLH();
        // set back the prevoius site rate's tree
        at(t)->getRate()->setTree(ptree);
        // cout << "[IQTreeMix::computeLikelihood] Tree " << t+1 << " : " << score << endl;
        pattern_lh_tree += nptn;
    }

    // reorganize the array
    i=0;
    for (t=0; t<ntree; t++) {
        j=t;
        for (ptn=0; ptn<nptn; ptn++) {
            ptn_like_cat[j] = exp(_ptn_like_cat[i]);
            i++;
            j+=ntree;
        }
    }
    
    // compute the total likelihood
    i=0;
    for (ptn=0; ptn<nptn; ptn++) {
        subLike = 0.0;
        for (t=0; t<ntree; t++) {
            subLike += ptn_like_cat[i] * weights[t];
            i++;
        }
        if (pattern_lh) {
            pattern_lh[ptn] = subLike;
        }
        // cout << ptn << "\t" << log(subLike) << "\t" << patn_freqs[ptn] << endl;
        logLike += log(subLike) * (double) patn_freqs[ptn];
    }
    // cout << "[IQTreeMix::computeLikelihood] log-likelihood: " << logLike << endl;

    return logLike;
}

/**
        compute pattern likelihoods only if the accumulated scaling factor is non-zero.
        Otherwise, copy the pattern_lh attribute
        @param pattern_lh (OUT) pattern log-likelihoods,
                        assuming pattern_lh has the size of the number of patterns
        @param cur_logl current log-likelihood (for sanity check)
        @param pattern_lh_cat (OUT) if not NULL, store all pattern-likelihood per category
 */
void IQTreeMix::computePatternLikelihood(double *pattern_lh, double *cur_logl,
                                         double *pattern_lh_cat, SiteLoglType wsl) {
    size_t i,ptn,t;
    size_t nptn,ntree;
    double subLike;

    computeLikelihood(pattern_lh);
}

void IQTreeMix::initializeAllPartialLh() {
    size_t i;
    // IQTree::initializeAllPartialLh();
    for (i=0; i<size(); i++) {
        at(i)->initializeAllPartialLh();
    }
}

void IQTreeMix::deleteAllPartialLh() {
    size_t i;
    // IQTree::deleteAllPartialLh();
    for (i=0; i<size(); i++) {
        at(i)->deleteAllPartialLh();
    }
}

void IQTreeMix::clearAllPartialLH(bool make_null) {
    size_t i;
    // IQTree::clearAllPartialLH(make_null);
    for (i=0; i<size(); i++) {
        at(i)->clearAllPartialLH(make_null);
    }
}

/**
        optimize all branch lengths of the tree
        @param iterations number of iterations to loop through all branches
        @return the likelihood of the tree
 */
double IQTreeMix::optimizeAllBranches(int my_iterations, double tolerance, int maxNRStep) {
    size_t i;
    PhyloTree* ptree;
    
    for (i=0; i<size(); i++) {
        cout << "[IQTreeMix::optimizeAllBranches] i = " << i << endl;
        // because the tree of the site rate is pointing to this tree
        // save the tree of the site rate
        ptree = at(i)->getRate()->getTree();
        at(i)->getRate()->setTree(at(i));
        at(i)->optimizeAllBranches(my_iterations, tolerance, maxNRStep);
        // restore the tree of the site rate
        at(i)->getRate()->setTree(ptree);
    }
    return computeLikelihood();
}

/**
        compute the updated tree weights according to the likelihood values along each site
        prerequisite: computeLikelihood() has been invoked

 */
double IQTreeMix::optimizeTreeWeightsByEM(double* pattern_mix_lh, int max_steps) {
    size_t nptn, ntree, ptn, c;
    double *this_lk_cat;
    double lk_ptn;
    double gradient_epsilon = 0.000001;
    double prev_score, score;
    int step;

    nptn = aln->getNPattern();
    ntree = size();
    
    initializeAllPartialLh();
    prev_score = computeLikelihood();
    clearAllPartialLH();
    
    for (step = 0; step < max_steps || max_steps == -1; step++) {
        
        getPostProb(pattern_mix_lh, false);

        // reset the weights
        for (c = 0; c < ntree; c++) {
            weights[c] = 0.0;
        }

        // E-step
        for (ptn = 0; ptn < nptn; ptn++) {
            this_lk_cat = pattern_mix_lh + ptn*ntree;
            for (c = 0; c < ntree; c++) {
                weights[c] += this_lk_cat[c];
            }
        }

        // M-step
        for (c = 0; c < ntree; c++) {
            weights[c] = weights[c] / getAlnNSite();
            if (weights[c] < 1e-10) weights[c] = 1e-10;
        }

        // show the weights
        cout << "[IQTreeMix::optimizeTreeWeights] " << step << " weights:";
        for (c = 0; c < ntree; c++) {
            if (c > 0)
                cout << ",";
            cout << weights[c];
        }
        cout << endl;

        initializeAllPartialLh();
        score = computeLikelihood();
        clearAllPartialLH();

        if (score < prev_score + gradient_epsilon) {
            // converged
            break;
        }
        prev_score = score;

    }
    return score;
}

/**
        compute the updated tree weights according to the likelihood values along each site
        prerequisite: computeLikelihood() has been invoked

 */
double IQTreeMix::optimizeTreeWeightsByBFGS() {
    double gradient_epsilon = 0.000001;
    int ndim = size();
    size_t i;
    double *variables = new double[ndim+1]; // used for BFGS numerical recipes
    double *upper_bound = new double[ndim+1];
    double *lower_bound = new double[ndim+1];
    bool *bound_check = new bool[ndim+1];
    double score;

    // initialize tmp_weights for optimzation
    tmp_weights.resize(size());
    for (i=0; i<size(); i++)
        tmp_weights[i] = weights[i];
    
    // by BFGS algorithm
    setVariables(variables);
    setBounds(lower_bound, upper_bound, bound_check);
    score = -minimizeMultiDimen(variables, ndim, lower_bound, upper_bound, bound_check, gradient_epsilon);
    getVariables(variables);

    delete[] variables;
    delete[] upper_bound;
    delete[] lower_bound;
    delete[] bound_check;
    
    //show the tree weights
    cout << "Tree weights: ";
    for (i=0; i<size(); i++) {
        cout << weights[i] << ";";
    }
    cout << endl;

    return score;
}

void IQTreeMix::showTree() {
    size_t i;
    for (i=0; i<size(); i++) {
        cout << "Tree " << i+1 << ": ";
        at(i)->printTree(cout);
        cout << endl;
    }
}

void IQTreeMix::setRootNode(const char *my_root, bool multi_taxa) {
    size_t i;
    for (i=0; i<size(); i++) {
        at(i)->setRootNode(my_root, multi_taxa);
    }
}

/**
    set checkpoint object
    @param checkpoint
*/
void IQTreeMix::setCheckpoint(Checkpoint *checkpoint) {
    size_t i;
    IQTree::setCheckpoint(checkpoint);
    for (i=0; i<size(); i++) {
        at(i)->setCheckpoint(checkpoint);
    }
}

void IQTreeMix::startCheckpoint() {
    checkpoint->startStruct("IQTreeMix" + convertIntToString(size()));
}

void IQTreeMix::saveCheckpoint() {
    size_t i;
    startCheckpoint();
    ASSERT(weights.size() == size());
    double* relative_weights = new double[size()];
    for (i=0; i<size(); i++) {
        relative_weights[i]=weights[i];
    }
    CKP_ARRAY_SAVE(size(), relative_weights);
    for (i=0; i<size(); i++) {
        checkpoint->startStruct("Tree" + convertIntToString(i+1));
        at(i)->saveCheckpoint();
        checkpoint->endStruct();
    }
    endCheckpoint();
    delete[] relative_weights;
}

void IQTreeMix::restoreCheckpoint() {
    size_t i;
    startCheckpoint();
    ASSERT(weights.size() == size());
    double* relative_weights = new double[size()];
    if (CKP_ARRAY_RESTORE(size(), relative_weights)) {
        for (i = 0; i < size(); i++)
            this->weights[i] = relative_weights[i];
    }
    for (i=0; i<size(); i++) {
        checkpoint->startStruct("Tree" + convertIntToString(i+1));
        at(i)->restoreCheckpoint();
        checkpoint->endStruct();
    }
    endCheckpoint();
    clearAllPartialLH();
    delete[] relative_weights;
}

void IQTreeMix::setMinBranchLen(Params& params) {
    size_t i;
    int num_prec;
    if (params.min_branch_length <= 0.0) {
        params.min_branch_length = 1e-6;
        if (size() > 0) {
            if (!at(0)->isSuperTree() && at(0)->getAlnNSite() >= 100000) {
                params.min_branch_length = 0.1 / (at(0)->getAlnNSite());
                num_prec = max((int)ceil(-log10(Params::getInstance().min_branch_length))+1, 6);
                for (i=0; i<size(); i++)
                    at(i)->num_precision = num_prec;
                cout.precision(12);
                cout << "NOTE: minimal branch length is reduced to " << params.min_branch_length << " for long alignment" << endl;
                cout.precision(3);
            }
        }
        // Increase the minimum branch length if PoMo is used.
        if (aln->seq_type == SEQ_POMO) {
            params.min_branch_length *= aln->virtual_pop_size * aln->virtual_pop_size;
            cout.precision(12);
            cout << "NOTE: minimal branch length is increased to " << params.min_branch_length << " because PoMo infers number of mutations and frequency shifts" << endl;
            cout.precision(3);
        }
    }
}

/** set pointer of params variable */
void IQTreeMix::setParams(Params* params) {
    size_t i;
    for (i=0; i<size(); i++) {
        at(i)->setParams(params);
    }
    this->params = params;
};

/**
 * Generate the initial tree (usually used for model parameter estimation)
 */
void IQTreeMix::computeInitialTree(LikelihoodKernel kernel, istream* in) {
    size_t i;
    ifstream fin;

    if (size() == 0)
        outError("No tree is inputted for the tree-mixture model");
    if (params->user_file == NULL) {
        outError("Tree file has to be inputed (using the option -te) for tree-mixture model");
    }
    
    fin.open(params->user_file);
    
    for (i=0; i<size(); i++) {
        at(i)->computeInitialTree(kernel, &fin);
    }
    
    fin.close();
    
    // show trees
    showTree();
}

/**
 * setup all necessary parameters
 */
void IQTreeMix::initSettings(Params &params) {
    size_t i;
    for (i=0; i<size(); i++) {
        at(i)->initSettings(params);
    }
}

uint64_t IQTreeMix::getMemoryRequired(size_t ncategory, bool full_mem) {
    uint64_t mem_size = 0;
    size_t i;
    for (i=0; i<size(); i++) {
        mem_size += at(i)->getMemoryRequired(ncategory, full_mem);
    }
    return mem_size;
}

// get memory requirement for ModelFinder
uint64_t IQTreeMix::getMemoryRequiredThreaded(size_t ncategory, bool full_mem) {
    // only get the largest k partitions (k=#threads)
    int threads = (params->num_threads != 0) ? params->num_threads : params->num_threads_max;
    threads = min(threads, countPhysicalCPUCores());
    threads = min(threads, (int)size());
    
    // sort partition by computational cost for OpenMP effciency
    uint64_t *part_mem = new uint64_t[size()];
    int i;
    for (i = 0; i < size(); i++) {
        part_mem[i] = at(i)->getMemoryRequired(ncategory, full_mem);
    }
    
    // sort partition memory in increasing order
    quicksort<uint64_t, int>(part_mem, 0, size()-1);
    
    uint64_t mem = 0;
    for (i = size()-threads; i < size(); i++)
        mem += part_mem[i];
    
    delete [] part_mem;
    
    return mem;
}

/**
    test the best number of threads
*/
int IQTreeMix::testNumThreads() {
    return at(0)->testNumThreads();
}

string IQTreeMix::optimizeModelParameters(bool printInfo, double logl_epsilon) {
    
    size_t i, ntree, nptn, ptn;
    int step, n;
    double* pattern_mix_lh;
    double gradient_epsilon = 0.0001;
    double prev_score = -DBL_MAX, score, tscore;
    PhyloTree *ptree;

    ntree = size();
    nptn = aln->getNPattern();
    
    // allocate memory
    pattern_mix_lh = new double[ntree * nptn];

    for (step = 0; step < optimize_steps || optimize_steps == -1; step++) {
        
        n = min(step+1,3);
        // n = 1;
    
        // get posterior probabilities along each site for each tree
        getPostProb(pattern_mix_lh, true);
        // update the ptn_freq array
        updateFreqArray(pattern_mix_lh);
        // optimize tree branches
        score = optimizeAllBranches(n, logl_epsilon);  // loop only 3 times in total
        cout << "after optimizing branches, likelihood = " << score << endl;
        
        // optimize tree weights
        // score = optimizeTreeWeights(pattern_mix_lh, -1);
        score = optimizeTreeWeightsByBFGS();
        cout << "after optimizing tree weights, likelihood = " << score << endl;

        // reset the ptn_freq array to original frequencies of the patterns
        for (i = 0; i < ntree; i++) {
            for (ptn = 0; ptn < nptn; ptn++) {
                at(i)->ptn_freq[ptn] = patn_freqs[ptn];
            }
        }
        
        for (i = 0; i < ntree; i++) {
            // save the site rate's tree
            ptree = at(i)->getRate()->getTree();
            // set this tree as the site rate's tree
            at(i)->getRate()->setTree(this);
            // at(i)->getModelFactory()->optimizeParametersOnly(n, gradient_epsilon, score);
            /* Optimize substitution and heterogeneity rates jointly using BFGS */
            tscore = at(i)->getModelFactory()->optimizeAllParameters(gradient_epsilon);
            if (tscore != 0.0)
                score = tscore;
            // set back the previous tree
            at(i)->getRate()->setTree(ptree);
        }

        
        /*
        // optimize the rate model parameters using BFGS
        cout << "optimizing the rate model parameters using BFGS" << endl;
        for (i=0; i<models.size(); i++) {
            if (models[i]) {
                score = models[i]->optimizeParameters(gradient_epsilon);
                cout << i+1 << " likelihood = " << score << endl;
            }
        }
    
        // optimize the site model parameters using BFGS
        if (anySiteRate) {
            cout << "optimizing the site model parameters using BFGS" << endl;
            for (i=0; i<site_rates.size(); i++) {
                if (site_rates[i]) {
                    score = site_rates[i]->optimizeParameters(gradient_epsilon);
                    cout << i+1 << " likelihood = " << score << endl;
                }
            }
        }
        */

        cout << "step= " << step << " score=" << score << endl;
        if (score < prev_score + gradient_epsilon) {
            // converged
            cout << "[IQTreeMix::optimizeModelParameters] coverged!" << endl;
            break;
        }
        prev_score = score;
    }

    setCurScore(score);

    delete[] pattern_mix_lh;
    
    return getTreeString();
}

/**
        print tree to .treefile
        @param params program parameters, field root is taken
 */
void IQTreeMix::printResultTree(string suffix) {
    ofstream fout;
    size_t i;
    
    if (MPIHelper::getInstance().isWorker()) {
        return;
    }
    if (params->suppress_output_flags & OUT_TREEFILE)
        return;
    string tree_file_name = params->out_prefix;
    tree_file_name += ".treefile";
    if (suffix.compare("") != 0) {
        tree_file_name += "." + suffix;
    }
    fout.open(tree_file_name.c_str());
    setRootNode(params->root, true);
    for (i=0; i<size(); i++)
        at(i)->printTree(fout);
    setRootNode(params->root, false);
    fout.close();
    if (verbose_mode >= VB_MED)
        cout << "Best tree printed to " << tree_file_name << endl;
}

string IQTreeMix::getTreeString() {
    stringstream tree_stream;
    size_t i;
    
    for (i=0; i<size(); i++)
        at(i)->printTree(tree_stream, WT_TAXON_ID + WT_BR_LEN + WT_SORT_TAXA);
    return tree_stream.str();
}

// return the average of the tree lengths
double IQTreeMix::treeLength(Node *node, Node *dad) {
    double len = 0.0;
    size_t i;
    for (i=0; i<size(); i++)
        len += at(i)->treeLength();
    return len / size();
}

// return the average length of all internal branches
double IQTreeMix::treeLengthInternal( double epsilon, Node *node, Node *dad) {
    double len = 0.0;
    size_t i;
    for (i = 0; i < size(); i++)
        len += at(i)->treeLengthInternal(epsilon);
    return len / size();
}

int IQTreeMix::getNParameters() {
    int df = 0;
    size_t i;
    for (i=0; i<models.size(); i++) {
        df += (models[i]->getNDim() + models[i]->getNDimFreq());
    }
    for (i=0; i<site_rates.size(); i++) {
        df += site_rates[i]->getNDim();
    }
    for (i=0; i<size(); i++) {
        df += at(i)->getNBranchParameters(BRLEN_OPTIMIZE);
    }
    // for tree weights
    df += (size() - 1);
    return df;
}

void IQTreeMix::drawTree(ostream &out, int brtype, double zero_epsilon) {
    size_t i;
    for (i=0; i<size(); i++) {
        out << "Tree " << i+1 << ":" << endl;
        at(i)->drawTree(out, brtype, zero_epsilon);
    }
}

/**
        print the tree to the output file in newick format
        @param out the output file.
        @param node the starting node, NULL to start from the root
        @param dad dad of the node, used to direct the search
        @param brtype type of branch to print
        @return ID of the taxon with smallest ID
 */
int IQTreeMix::printTree(ostream &out, int brtype, Node *node, Node *dad) {
    size_t i;
    int value = 0;
    for (i=0; i<size(); i++) {
        out << "Tree " << i+1 << ":" << endl;
        value = at(i)->printTree(out, brtype, node, dad);
    }
    return value;
}

/**
 *  @brief: either optimize model parameters on the current tree
 *  or restore them from a checkpoint (this function exists because the
 *  same things need to be done in two different places, in runTreeReconstruction)
 *  @param initEpsilon likelihood epsilon for optimization
 */

string IQTreeMix::ensureModelParametersAreSet(double initEpsilon) {
    string all_initTrees = "";
    string initTree;
    size_t i;
    for (i=0; i<size(); i++) {
        initTree = at(i)->ensureModelParametersAreSet(initEpsilon);
        if (all_initTrees.length() > 0)
            all_initTrees.append(";");
        all_initTrees.append(initTree);
    }
    cout << "[IQTreeMix::ensureModelParametersAreSet] all_initTrees = " << all_initTrees << endl << flush;
    return all_initTrees;
}

// get posterior probabilities along each site for each tree
void IQTreeMix::getPostProb(double* pattern_mix_lh, bool need_computeLike) {
    size_t ntree, nptn, i, ptn, c;
    double* this_lk_cat;
    double lk_ptn;

    ntree = size();
    nptn = aln->getNPattern();

    if (need_computeLike) {
        initializeAllPartialLh();
        computeLikelihood();
        clearAllPartialLH();
    }

    memcpy(pattern_mix_lh, ptn_like_cat, nptn*ntree*sizeof(double));

    // multiply pattern_mix_lh by tree weights
    i = 0;
    for (ptn = 0; ptn < nptn; ptn++) {
        for (c = 0; c < ntree; c++) {
            pattern_mix_lh[i] *= weights[c];
            i++;
        }
    }

    for (ptn = 0; ptn < nptn; ptn++) {
        this_lk_cat = pattern_mix_lh + ptn*ntree;
        lk_ptn = 0.0;
        for (c = 0; c < ntree; c++) {
            lk_ptn += this_lk_cat[c];
        }
        ASSERT(lk_ptn != 0.0);
        lk_ptn = patn_freqs[ptn] / lk_ptn;

        // transform pattern_mix_lh into posterior probabilities of each category
        for (c = 0; c < ntree; c++) {
            this_lk_cat[c] *= lk_ptn;
        }
    }
}

// update the ptn_freq array
void IQTreeMix::updateFreqArray(double* pattern_mix_lh) {
    size_t i, ptn;
    PhyloTree* tree;
    size_t ntree = size();
    size_t nptn = aln->getNPattern();

    for (i = 0; i < ntree; i++) {
        tree = at(i);
        // initialize likelihood
        tree->initializeAllPartialLh();
        // copy posterior probability into ptn_freq
        tree->computePtnFreq();
        double *this_lk_cat = pattern_mix_lh+i;
        for (ptn = 0; ptn < nptn; ptn++)
            tree->ptn_freq[ptn] = this_lk_cat[ptn*ntree];
    }
}


double IQTreeMix::targetFunk(double x[]) {
    getVariables(x);
    clearAllPartialLH();
    return -computeLikelihood();
}

// read the tree weights and write into "variables"
void IQTreeMix::setVariables(double *variables) {
    // for tree weights
    size_t i;
    for (i=0; i<size(); i++) {
        variables[i+1] = tmp_weights[i];
    }
}

// read the "variables" and write into tree weights
void IQTreeMix::getVariables(double *variables) {
    // for tree weights
    size_t i;
    double sum = 0.0;
    for (i=0; i<size(); i++) {
        tmp_weights[i] = variables[i+1];
        sum += tmp_weights[i];
    }
    for (i=0; i<size(); i++) {
        weights[i] = tmp_weights[i] / sum;
    }
}

// set the bounds
void IQTreeMix::setBounds(double *lower_bound, double *upper_bound, bool* bound_check) {
    size_t i;
    for (i=0; i<size(); i++) {
        lower_bound[i+1] = MIN_PROP;
        upper_bound[i+1] = MAX_PROP;
        bound_check[i+1] = true;
    }
}

// get the dimension of the variables (for tree weights)
int IQTreeMix::getNDim() {
    return size();
}
