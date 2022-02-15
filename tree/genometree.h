//
//  genometree.h
//  tree
//
//  Created by Nhan Ly-Trong on 09/02/2022.
//
#ifndef GENOMETREE_H
#define GENOMETREE_H

#include "genomenode.h"
#include "utils/timeutil.h"
#include <queue>
using namespace std;

/**
A Genome Tree to present a genome by genome entry (each is a set of sites)
 */
class GenomeTree {
private:
    /**
        find a node that contains a given position
     */
    GenomeNode* findNodeByPos(GenomeNode* node, Insertion* insertion, int num_cumulative_gaps);
    
    /**
        insert gaps into a "all-gap" node
     */
    void insertGapsIntoGaps(GenomeNode* node, int length);
    
    /**
        insert gaps into a "normal" (all sites from the original genome) node
     */
    void insertGapsIntoNormalNode(GenomeNode* node, int pos, int length);
    
    /**
        update the cumulative_gaps_from_left_child of all nodes on the path from the current node to root
     */
    void updateCumulativeGapsFromLeftChild(GenomeNode* node, int length);
    
public:
    /**
        starting pos in the original genome
     */
    GenomeNode* root;

    /**
        constructor
     */
    GenomeTree();
    
    /**
        init a root genome node
     */
    GenomeTree(int length);
    
    /**
        deconstructor
     */
    ~GenomeTree();
    
    /**
        update genome tree from an insertion forward the insertion list
     */
    void updateTree(Insertion* insertion);
    
    /**
        export new genome from original genome and genome tree
     */
    vector<short int> exportNewGenome(vector<short int> ori_seq, int seq_length, int UNKOWN_STATE);
    
    /**
     export readable characters (for writing to file) from original genome and genome tree
     */
    void exportReadableCharacters(vector<short int> ori_seq, int num_sites_per_state, vector<string> state_mapping, string &output);

};
#endif