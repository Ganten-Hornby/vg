//#define debug_distance_indexing
//#define debug_snarl_traversal
#define debug_distances
#define debug_subgraph

#include "snarl_distance_index.hpp"

using namespace std;
using namespace handlegraph;
namespace vg {


void fill_in_distance_index(SnarlDistanceIndex* distance_index, const HandleGraph* graph, const HandleGraphSnarlFinder* snarl_finder, size_t size_limit) {
    distance_index->set_snarl_size_limit(size_limit);

    //Build the temporary distance index from the graph
    SnarlDistanceIndex::TemporaryDistanceIndex temp_index = make_temporary_distance_index(graph, snarl_finder, size_limit);

    //And fill in the permanent distance index
    vector<const SnarlDistanceIndex::TemporaryDistanceIndex*> indexes;
    indexes.emplace_back(&temp_index);
    distance_index->get_snarl_tree_records(indexes, graph);
}
SnarlDistanceIndex::TemporaryDistanceIndex make_temporary_distance_index(
    const HandleGraph* graph, const HandleGraphSnarlFinder* snarl_finder, size_t size_limit)  {

#ifdef debug_distance_indexing
    cerr << "Creating new distance index for nodes between " << graph->min_node_id() << " and " << graph->max_node_id() << endl;

#endif

    SnarlDistanceIndex::TemporaryDistanceIndex temp_index;

    temp_index.min_node_id=graph->min_node_id();
    temp_index.max_node_id=graph->max_node_id();

    //Construct the distance index using the snarl decomposition
    //traverse_decomposition will visit all structures (including trivial snarls), calling
    //each of the given functions for the start and ends of the snarls and chains

    temp_index.temp_node_records.resize(temp_index.max_node_id-temp_index.min_node_id+1);



    //Stores unfinished records, as type of record and offset into appropriate vector
    //(temp_node/snarl/chain_records)
    vector<pair<SnarlDistanceIndex::temp_record_t, size_t>> stack;

    //There may be components of the root that are connected to each other. Each connected component will
    //get put into a (fake) root-level snarl, but we don't know what those components will be initially,
    //since the decomposition just puts them in the same root snarl. This is used to group the root-level
    //components into connected components that will later be used to make root snarls
    structures::UnionFind root_snarl_component_uf (0);


    /*Go through the decomposition top down and record the connectivity of the snarls and chains
     * Distances will be added later*/

    snarl_finder->traverse_decomposition(
    [&](handle_t chain_start_handle) {
        /*This gets called when a new chain is found, starting at the start handle going into chain
         * For the first node in a chain, create a chain record and fill in the first node.
         * Also add the first node record
         */
#ifdef debug_distance_indexing
        cerr << "  Starting new chain at " << graph->get_id(chain_start_handle) << (graph->get_is_reverse(chain_start_handle) ? " reverse" : " forward") << endl;
        //We shouldn't have seen this node before
        //assert(temp_index.temp_node_records[graph->get_id(chain_start_handle)-min_node_id].node_id == 0);
#endif

        //Fill in node in chain
        stack.emplace_back(SnarlDistanceIndex::TEMP_CHAIN, temp_index.temp_chain_records.size());
        nid_t node_id = graph->get_id(chain_start_handle);
        temp_index.temp_chain_records.emplace_back();
        auto& temp_chain = temp_index.temp_chain_records.back();
        temp_chain.start_node_id = node_id; 
        temp_chain.start_node_rev = graph->get_is_reverse(chain_start_handle);
        temp_chain.children.emplace_back(SnarlDistanceIndex::TEMP_NODE, node_id);


        //And the node record itself
        auto& temp_node = temp_index.temp_node_records.at(node_id-temp_index.min_node_id);
        temp_node.node_id = node_id;
        temp_node.node_length = graph->get_length(chain_start_handle);
        temp_node.reversed_in_parent = graph->get_is_reverse(chain_start_handle);
        temp_node.parent = stack.back(); //The parent is this chain

    },
    [&](handle_t chain_end_handle) {
        /*This gets called at the end of a chain, facing out
         * Record the chain's end node. The node record itself would have been added as part of the snarl
         * Also record the chain's parent here
         */

        //Done with this chain
        pair<SnarlDistanceIndex::temp_record_t, size_t> chain_index = stack.back();
        stack.pop_back();

        assert(chain_index.first == SnarlDistanceIndex::TEMP_CHAIN);
        SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryChainRecord& temp_chain_record = temp_index.temp_chain_records.at(chain_index.second);
        nid_t node_id = graph->get_id(chain_end_handle);

        if (temp_chain_record.children.size() == 1 && node_id == temp_chain_record.start_node_id) {
            //This is a trivial snarl

            //Then this must be the last thing on the chain_records vector
            assert(temp_index.temp_chain_records.size() == chain_index.second+1);

            //Get the node
            SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& temp_node_record = temp_index.temp_node_records.at(node_id - temp_index.min_node_id);

            temp_node_record.reversed_in_parent = false;

            //And give the chain's parent the node info
            //
            if (stack.empty()) {
                temp_node_record.parent = make_pair(SnarlDistanceIndex::TEMP_ROOT, 0);
                //If this was the last thing on the stack, then this was a root

                //Check to see if there is anything connected to the ends of the chain
                vector<nid_t> reachable_nodes;
                graph->follow_edges(graph->get_handle(node_id, false),
                    false, [&] (const handle_t& next) {
                        if (graph->get_id(next) != node_id) {
                            reachable_nodes.emplace_back(graph->get_id(next));
                        }
                    });
                graph->follow_edges(graph->get_handle(node_id, true),
                    false, [&] (const handle_t& next) {
                        if (graph->get_id(next) != node_id) {
                            reachable_nodes.emplace_back(graph->get_id(next));
                        }
                    });
                if (reachable_nodes.size()) {
                    //If we can reach anything leaving the chain (besides the chain itself), then it is part of a root snarl
                    //Note that if the chain's start and end node are the same, then it will always be a single component
#ifdef debug_distance_indexing
                    cerr << "                 This trivial chain is part of the root but connects with something else in the root"<<endl;
#endif
                    bool new_component = true;

                    //Add this to the union find
                    root_snarl_component_uf.resize(root_snarl_component_uf.size() + 1);
                    //And remember that it's in a connected component of the root
                    temp_node_record.root_snarl_index = temp_index.root_snarl_components.size();
                    temp_index.root_snarl_components.emplace_back(SnarlDistanceIndex::TEMP_NODE, node_id);
                    for (nid_t next_id : reachable_nodes) {
                        //For each node that this is connected to, check if we've already seen it and if we have, then
                        //union this chain and that node's chain
                        SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& node_record = temp_index.temp_node_records[next_id-temp_index.min_node_id];
                        if (node_record.node_id != 0) {
                            //If we've already seen this node, union it with the new one
                            //If we can see it by walking out from this top-level chain, then it must also be a
                            //top-level chain (or node pretending to be a chain)
                            size_t other_i = node_record.parent.first == SnarlDistanceIndex::TEMP_CHAIN
                                           ? temp_index.temp_chain_records[node_record.parent.second].root_snarl_index
                                           : node_record.root_snarl_index;
                            assert(other_i != std::numeric_limits<size_t>::max());
                            root_snarl_component_uf.union_groups(other_i, temp_node_record.root_snarl_index);
//#ifdef debug_distance_indexing
//                            cerr << "        Union this trivial  with " << temp_index.temp_chain_records[node_record.parent.second].start_node_id << " " << temp_index.temp_chain_records[node_record.parent.second].end_node_id << endl;
//#endif
                        } else {
                            new_component = false;
                        }
                    }
                } else {
                    //If this chain isn't connected to anything else, then it is a single component of the root
                    temp_index.components.emplace_back(SnarlDistanceIndex::TEMP_NODE, node_id);
                }
            } else {
                //The last thing on the stack is the parent of this chain, which must be a snarl
                temp_node_record.parent = stack.back();
                auto& parent_snarl_record = temp_index.temp_snarl_records.at(temp_node_record.parent.second);
                temp_node_record.rank_in_parent = parent_snarl_record.children.size() + 2;
                parent_snarl_record.children.emplace_back(SnarlDistanceIndex::TEMP_NODE, node_id);
            }


            //Remove the chain record
            temp_index.temp_chain_records.pop_back();
            temp_index.max_index_size += temp_node_record.get_max_record_length();

        } else {
            //Otherwise, it is an actual chain

            //Fill in node in chain
            temp_chain_record.end_node_id = node_id;
            temp_chain_record.end_node_rev = graph->get_is_reverse(chain_end_handle);
            temp_chain_record.end_node_length = graph->get_length(chain_end_handle);

            if (stack.empty()) {
                //If this was the last thing on the stack, then this was a root

                //Check to see if there is anything connected to the ends of the chain
                vector<nid_t> reachable_nodes;
                graph->follow_edges(graph->get_handle(temp_chain_record.start_node_id, !temp_chain_record.start_node_rev),
                    false, [&] (const handle_t& next) {
                        if (graph->get_id(next) != temp_chain_record.start_node_id &&
                            graph->get_id(next) != temp_chain_record.end_node_id) {
                            reachable_nodes.emplace_back(graph->get_id(next));
                        }
                    });
                graph->follow_edges(graph->get_handle(temp_chain_record.end_node_id, temp_chain_record.end_node_rev),
                    false, [&] (const handle_t& next) {
                        if (graph->get_id(next) != temp_chain_record.start_node_id &&
                            graph->get_id(next) != temp_chain_record.end_node_id) {
                            reachable_nodes.emplace_back(graph->get_id(next));
                        }
                    });
                if (reachable_nodes.size() && (temp_chain_record.is_trivial || temp_chain_record.start_node_id != temp_chain_record.end_node_id)) {
                    //If we can reach anything leaving the chain (besides the chain itself), then it is part of a root snarl
                    //Note that if the chain's start and end node are the same, then it will always be a single component
#ifdef debug_distance_indexing
                    cerr << "                 This chain is part of the root but connects with something else in the root"<<endl;
#endif
                    bool new_component = true;

                    //Add this to the union find
                    root_snarl_component_uf.resize(root_snarl_component_uf.size() + 1);
                    //And remember that it's in a connected component of the root
                    temp_chain_record.root_snarl_index = temp_index.root_snarl_components.size();
                    temp_index.root_snarl_components.emplace_back(chain_index);
                    for (nid_t next_id : reachable_nodes) {
                        //For each node that this is connected to, check if we've already seen it and if we have, then
                        //union this chain and that node's chain
                        SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& node_record = temp_index.temp_node_records[next_id-temp_index.min_node_id];
                        if (node_record.node_id != 0) {
                            //If we've already seen this node, union it with the new one
                            //If we can see it by walking out from this top-level chain, then it must also be a
                            //top-level chain (or node pretending to be a chain)
                            size_t other_i = node_record.parent.first == SnarlDistanceIndex::TEMP_CHAIN
                                           ? temp_index.temp_chain_records[node_record.parent.second].root_snarl_index
                                           : node_record.root_snarl_index;
                            assert(other_i != std::numeric_limits<size_t>::max());
                            root_snarl_component_uf.union_groups(other_i, temp_chain_record.root_snarl_index);
#ifdef debug_distance_indexing
                            cerr << "        Union this chain with " << temp_index.temp_chain_records[node_record.parent.second].start_node_id << " " << temp_index.temp_chain_records[node_record.parent.second].end_node_id << endl;
#endif
                        } else {
                            new_component = false;
                        }
                    }
                } else {
                    //If this chain isn't connected to anything else, then it is a single component of the root
                    temp_chain_record.parent = make_pair(SnarlDistanceIndex::TEMP_ROOT, 0);
                    temp_index.components.emplace_back(chain_index);
                }
            } else {
                //The last thing on the stack is the parent of this chain, which must be a snarl
                temp_chain_record.parent = stack.back();
                auto& parent_snarl_record = temp_index.temp_snarl_records.at(temp_chain_record.parent.second);
                temp_chain_record.rank_in_parent = parent_snarl_record.children.size() + 2;
                parent_snarl_record.children.emplace_back(chain_index);
            }

        temp_index.max_index_size += temp_chain_record.get_max_record_length();
#ifdef debug_distance_indexing
            cerr << "  Ending new " << (temp_chain_record.is_trivial ? "trivial " : "") <<  "chain " << temp_index.structure_start_end_as_string(chain_index)
              << endl << "    that is a child of " << temp_index.structure_start_end_as_string(temp_chain_record.parent) << endl;
#endif
        }
    },
    [&](handle_t snarl_start_handle) {
        /*This gets called at the beginning of a new snarl facing in
         * Create a new snarl record and fill in the start node.
         * The node record would have been created as part of the chain, or as the end node
         * of the previous snarl
         */

#ifdef debug_distance_indexing
        cerr << "  Starting new snarl at " << graph->get_id(snarl_start_handle) << (graph->get_is_reverse(snarl_start_handle) ? " reverse" : " forward") << endl;
        cerr << "with index " << temp_index.temp_snarl_records.size() << endl;
#endif
        auto& parent = stack.back();
        stack.emplace_back(SnarlDistanceIndex::TEMP_SNARL, temp_index.temp_snarl_records.size());
        temp_index.temp_snarl_records.emplace_back();
        temp_index.temp_snarl_records.back().start_node_id = graph->get_id(snarl_start_handle);
        temp_index.temp_snarl_records.back().start_node_rev = graph->get_is_reverse(snarl_start_handle);
        temp_index.temp_snarl_records.back().start_node_length = graph->get_length(snarl_start_handle);

    },
    [&](handle_t snarl_end_handle){
        /*This gets called at the end of the snarl facing out
         * Fill in the end node of the snarl, its parent, and record the snarl as a child of its
         * parent chain
         * Also create a node record
         */
        pair<SnarlDistanceIndex::temp_record_t, size_t> snarl_index = stack.back();
        stack.pop_back();
        assert(snarl_index.first == SnarlDistanceIndex::TEMP_SNARL);
        assert(stack.back().first == SnarlDistanceIndex::TEMP_CHAIN);
        SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records[snarl_index.second];
        nid_t node_id = graph->get_id(snarl_end_handle);

        //Record the end node in the snarl
        temp_snarl_record.end_node_id = node_id;
        temp_snarl_record.end_node_rev = graph->get_is_reverse(snarl_end_handle);
        temp_snarl_record.end_node_length = graph->get_length(snarl_end_handle);
        temp_snarl_record.node_count = temp_snarl_record.children.size();
        bool any_edges_in_snarl = false;
        graph->follow_edges(graph->get_handle(temp_snarl_record.start_node_id, temp_snarl_record.start_node_rev), false, [&](const handle_t next_handle) {
            if (graph->get_id(next_handle) != temp_snarl_record.end_node_id) {
                any_edges_in_snarl = true;
            }
        });
        graph->follow_edges(graph->get_handle(temp_snarl_record.end_node_id, !temp_snarl_record.end_node_rev), false, [&](const handle_t next_handle) {
            if (graph->get_id(next_handle) != temp_snarl_record.start_node_id) {
                any_edges_in_snarl = true;
            }
        });

        if (temp_snarl_record.children.size() == 0) {
            //This is a trivial snarl
            temp_snarl_record.is_trivial = true;

            //Add the end node to the chain
            assert(stack.back().first == SnarlDistanceIndex::TEMP_CHAIN);
            temp_snarl_record.parent = stack.back();
            auto& temp_chain = temp_index.temp_chain_records.at(stack.back().second);
            temp_chain.children.emplace_back(SnarlDistanceIndex::TEMP_NODE, node_id);

            //Remove the snarl record
            assert(temp_index.temp_snarl_records.size() == snarl_index.second+1);
            temp_index.temp_snarl_records.pop_back();
        } else {
            //This is the child of a chain
            assert(stack.back().first == SnarlDistanceIndex::TEMP_CHAIN);
            temp_snarl_record.parent = stack.back();
            auto& temp_chain = temp_index.temp_chain_records.at(stack.back().second);
            temp_chain.children.emplace_back(snarl_index);
            temp_chain.children.emplace_back(SnarlDistanceIndex::TEMP_NODE, node_id);

        }
        //Record the snarl as a child of its chain
        //if (stack.empty()) {
        //    assert(false);
        //    //TODO: The snarl should always be the child of a chain
        //    //If this was the last thing on the stack, then this was a root
        //    //TODO: I'm not sure if this would get put into a chain or not
        //    temp_snarl_record.parent = make_pair(SnarlDistanceIndex::TEMP_ROOT, 0);
        //    temp_index.components.emplace_back(snarl_index);
        //} 

        //Record the node itself. This gets done for the start of the chain, and ends of snarls
        SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& temp_node_record = temp_index.temp_node_records.at(node_id-temp_index.min_node_id);
        temp_node_record.node_id = node_id;
        temp_node_record.node_length = graph->get_length(snarl_end_handle);
        temp_node_record.reversed_in_parent = graph->get_is_reverse(snarl_end_handle);
        temp_node_record.parent = stack.back();



#ifdef debug_distance_indexing
        cerr << "  Ending new snarl " << temp_index.structure_start_end_as_string(snarl_index)
             << endl << "    that is a child of " << temp_index.structure_start_end_as_string(temp_snarl_record.parent) << endl;
#endif
    });

    /*
     * We finished going through everything that exists according to the snarl decomposition, but
     * it's still missing tips, which will be discovered when filling in the snarl distances,
     * and root-level snarls, which we'll add now by combining the chain components in root_snarl_components
     * into snarls defined by root_snarl_component_uf
     * The root-level snarl is a fake snarl that doesn't exist according to the snarl decomposition,
     * but is an extra layer that groups together components of the root that are connected
     */

    vector<vector<size_t>> root_snarl_component_indexes = root_snarl_component_uf.all_groups();
    for (vector<size_t>& root_snarl_indexes : root_snarl_component_indexes) {
#ifdef debug_distance_indexing
        cerr << "Create a new root snarl from components" << endl;
#endif
        //For each of the root snarls
        temp_index.components.emplace_back(SnarlDistanceIndex::TEMP_SNARL, temp_index.temp_snarl_records.size());
        temp_index.temp_snarl_records.emplace_back();
        SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records.back();
        temp_snarl_record.is_root_snarl = true;
        temp_snarl_record.parent = make_pair(SnarlDistanceIndex::TEMP_ROOT, 0);


        for (size_t chain_i : root_snarl_indexes) {
            //For each chain component of this root-level snarl
            if (temp_index.root_snarl_components[chain_i].first == SnarlDistanceIndex::TEMP_CHAIN){
                SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryChainRecord& temp_chain_record = temp_index.temp_chain_records[temp_index.root_snarl_components[chain_i].second];
                temp_chain_record.parent = make_pair(SnarlDistanceIndex::TEMP_SNARL, temp_index.temp_snarl_records.size() - 1);
                temp_chain_record.rank_in_parent = temp_snarl_record.children.size();
                temp_chain_record.reversed_in_parent = false;

                temp_snarl_record.children.emplace_back(temp_index.root_snarl_components[chain_i]);
            } else {
                assert(temp_index.root_snarl_components[chain_i].first == SnarlDistanceIndex::TEMP_NODE);
                SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& temp_node_record = temp_index.temp_node_records[temp_index.root_snarl_components[chain_i].second - temp_index.min_node_id];
                temp_node_record.parent = make_pair(SnarlDistanceIndex::TEMP_SNARL, temp_index.temp_snarl_records.size() - 1);
                temp_node_record.rank_in_parent = temp_snarl_record.children.size();
                temp_node_record.reversed_in_parent = false;

                temp_snarl_record.children.emplace_back(temp_index.root_snarl_components[chain_i]);
            }
        }
        temp_snarl_record.node_count = temp_snarl_record.children.size();
    }

    /*Now go through the decomposition again to fill in the distances
     * This traverses all chains in reverse order that we found them in, so bottom up
     * Each chain and snarl already knows its parents and children, except for single nodes
     * that are children of snarls. These nodes were not in chains will have their node
     * records created here
     */

#ifdef debug_distance_indexing
    cerr << "Filling in the distances in snarls" << endl;
#endif
    for (int i = temp_index.temp_chain_records.size()-1 ; i >= 0 ; i--) {

        SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryChainRecord& temp_chain_record = temp_index.temp_chain_records[i];
        assert(!temp_chain_record.is_trivial);
#ifdef debug_distance_indexing
        cerr << "  At "  << (temp_chain_record.is_trivial ? " trivial " : "") << " chain " << temp_index.structure_start_end_as_string(make_pair(SnarlDistanceIndex::TEMP_CHAIN, i)) << endl;
#endif

        //Add the first values for the prefix sum and backwards loop vectors
        temp_chain_record.prefix_sum.emplace_back(0);
        temp_chain_record.backward_loops.emplace_back(std::numeric_limits<size_t>::max());
        temp_chain_record.chain_components.emplace_back(0);


        /*First, go through each of the snarls in the chain in the forward direction and
         * fill in the distances in the snarl. Also fill in the prefix sum and backwards
         * loop vectors here
         */
        size_t curr_component = 0; //which component of the chain are we in
        size_t last_node_length = 0;
        for (size_t chain_child_i = 0 ; chain_child_i < temp_chain_record.children.size() ; chain_child_i++ ){
            const pair<SnarlDistanceIndex::temp_record_t, size_t>& chain_child_index = temp_chain_record.children[chain_child_i];
            //Go through each of the children in the chain, skipping nodes
            //The snarl may be trivial, in which case don't fill in the distances
#ifdef debug_distance_indexing
            cerr << "    Looking at child " << temp_index.structure_start_end_as_string(chain_child_index) << endl;
#endif

            if (chain_child_index.first == SnarlDistanceIndex::TEMP_SNARL){
                //This is where all the work gets done. Need to go through the snarl and add
                //all distances, then add distances to the chain that this is in
                //The parent chain will be the last thing in the stack
                SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = 
                        temp_index.temp_snarl_records.at(chain_child_index.second);

                //Fill in this snarl's distances
                populate_snarl_index(temp_index, chain_child_index, size_limit, graph);

                bool new_component = temp_snarl_record.min_length == std::numeric_limits<size_t>::max();
                if (new_component){
                    curr_component++;
                }

                //And get the distance values for the end node of the snarl in the chain
                if (new_component) {
                    //If this snarl wasn't start-end connected, then we start 
                    //tracking the distance vectors here

                    //Update the maximum distance
                    temp_index.max_distance = std::max(temp_index.max_distance, temp_chain_record.prefix_sum.back());

                    //If this is the second component of the multicomponent chain, then remember the minimum length
                    if (curr_component == 1) {
                        temp_chain_record.min_length = temp_chain_record.prefix_sum.back();
                    }
                    temp_chain_record.prefix_sum.emplace_back(0);
                    temp_chain_record.backward_loops.emplace_back(temp_snarl_record.loop_end);
                    //If the chain is disconnected, the max length is infinite
                    temp_chain_record.max_length =  std::numeric_limits<size_t>::max();
                } else {
                    temp_chain_record.prefix_sum.emplace_back(SnarlDistanceIndex::sum({temp_chain_record.prefix_sum.back(),
                        temp_snarl_record.min_length, temp_snarl_record.start_node_length}));
                    temp_chain_record.backward_loops.emplace_back(std::min(temp_snarl_record.loop_end,
                        SnarlDistanceIndex::sum({temp_chain_record.backward_loops.back()
                        , 2 * (temp_snarl_record.start_node_length + temp_snarl_record.min_length)})));
                    temp_chain_record.max_length = SnarlDistanceIndex::sum({temp_chain_record.max_length,
                                                                           temp_snarl_record.max_length});
                }
                temp_chain_record.chain_components.emplace_back(curr_component);
                if (chain_child_i == temp_chain_record.children.size() - 2 && temp_snarl_record.min_length == std::numeric_limits<size_t>::max()) {
                    temp_chain_record.loopable = false;
                }
                last_node_length = 0;
            } else {
                if (last_node_length != 0) {
                    //If this is a node and the last thing was also a node,
                    //then there was a trivial snarl 
                    SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& temp_node_record = 
                            temp_index.temp_node_records.at(chain_child_index.second-temp_index.min_node_id);

                    //Check if there is a loop in this node
                    //Snarls get counted as trivial if they contain no nodes but they might still have edges
                    size_t backward_loop = std::numeric_limits<size_t>::max();

                    graph->follow_edges(graph->get_handle(temp_node_record.node_id, !temp_node_record.reversed_in_parent), false, [&](const handle_t next_handle) {
                        if (graph->get_id(next_handle) == temp_node_record.node_id) {
                            //If there is a loop going backwards (relative to the chain) back to the same node
                            backward_loop = 0;
                        }
                    });

                    temp_chain_record.prefix_sum.emplace_back(SnarlDistanceIndex::sum({temp_chain_record.prefix_sum.back(), last_node_length}));
                    temp_chain_record.backward_loops.emplace_back(std::min(backward_loop,
                        SnarlDistanceIndex::sum({temp_chain_record.backward_loops.back(), 2 * last_node_length})));

                    if (chain_child_i == temp_chain_record.children.size()-1) {
                        //If this is the last node
                        temp_chain_record.loopable=false;
                    }
                    temp_chain_record.chain_components.emplace_back(curr_component);
                }
                last_node_length = temp_index.temp_node_records.at(chain_child_index.second - temp_index.min_node_id).node_length;
                //And update the chains max length
                temp_chain_record.max_length = SnarlDistanceIndex::sum({temp_chain_record.max_length,
                                                                       last_node_length});
            }
        } //Finished walking through chain
        if (temp_chain_record.start_node_id == temp_chain_record.end_node_id && temp_chain_record.chain_components.back() != 0) {
            //If this is a looping, multicomponent chain, the start/end node could end up in separate chain components
            //despite being the same node.
            //Since the first component will always be 0, set the first node's component to be whatever the last
            //component was
            temp_chain_record.chain_components[0] = temp_chain_record.chain_components.back();

        }
        temp_chain_record.min_length = !temp_chain_record.is_trivial && temp_chain_record.start_node_id == temp_chain_record.end_node_id
                        ? SnarlDistanceIndex::sum({temp_chain_record.prefix_sum.back(), temp_chain_record.min_length})
                        : SnarlDistanceIndex::sum({temp_chain_record.prefix_sum.back() , temp_chain_record.end_node_length});

        assert(temp_chain_record.prefix_sum.size() == temp_chain_record.backward_loops.size());
        assert(temp_chain_record.prefix_sum.size() == temp_chain_record.chain_components.size());


        /*Now that we've gone through all the snarls in the chain, fill in the forward loop vector
         * by going through the chain in the backwards direction
         */
        temp_chain_record.forward_loops.resize(temp_chain_record.prefix_sum.size(),
                                               std::numeric_limits<size_t>::max());
        if (temp_chain_record.start_node_id == temp_chain_record.end_node_id && temp_chain_record.children.size() > 1) {

            //If this is a looping chain, then check the first snarl for a loop
            if (temp_chain_record.children.at(1).first == SnarlDistanceIndex::TEMP_SNARL) {
                SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records.at(temp_chain_record.children.at(1).second);
                temp_chain_record.forward_loops[temp_chain_record.forward_loops.size()-1] = temp_snarl_record.loop_start;
            } 
        }

        size_t node_i = temp_chain_record.prefix_sum.size() - 2;
        // We start at the next to last node because we need to look at this record and the next one.
        last_node_length = 0;
        for (int j = (int)temp_chain_record.children.size() - 1 ; j >= 0 ; j--) {
            auto& child = temp_chain_record.children.at(j);
            if (child.first == SnarlDistanceIndex::TEMP_SNARL){
                SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records.at(child.second);
                if (temp_chain_record.chain_components.at(node_i) != temp_chain_record.chain_components.at(node_i+1) &&
                    temp_chain_record.chain_components.at(node_i+1) != 0){
                    //If this is a new chain component, then add the loop distance from the snarl
                    //If the component of the next node is 0, then we're still in the same component since we're going backwards
                    temp_chain_record.forward_loops.at(node_i) = temp_snarl_record.loop_start;
                } else {
                    temp_chain_record.forward_loops.at(node_i) =
                        std::min(SnarlDistanceIndex::sum({temp_chain_record.forward_loops.at(node_i+1) , 2* temp_snarl_record.min_length,
                                      2*temp_snarl_record.end_node_length}), temp_snarl_record.loop_start);
                }
                node_i --;
                last_node_length = 0;
            } else {
                if (last_node_length != 0) {
                    SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& temp_node_record = 
                            temp_index.temp_node_records.at(child.second-temp_index.min_node_id);


                    //Check if there is a loop in this node
                    //Snarls get counted as trivial if they contain no nodes but they might still have edges
                    size_t forward_loop = std::numeric_limits<size_t>::max();
                    graph->follow_edges(graph->get_handle(temp_node_record.node_id, temp_node_record.reversed_in_parent), false, [&](const handle_t next_handle) {
                        if (graph->get_id(next_handle) == temp_node_record.node_id) {
                            //If there is a loop going forward (relative to the chain) back to the same node
                            forward_loop = 0;
                        }
                    });
                    temp_chain_record.forward_loops.at(node_i) = std::min( forward_loop,
                        SnarlDistanceIndex::sum({temp_chain_record.forward_loops.at(node_i+1) , 
                                                 2*last_node_length}));
                    node_i--;
                }
                last_node_length = temp_index.temp_node_records.at(child.second - temp_index.min_node_id).node_length;
            }
        }


        //If this is a looping chain, check if the loop distances can be improved by going around the chain

        if (temp_chain_record.start_node_id == temp_chain_record.end_node_id && temp_chain_record.children.size() > 1) {


            //Also check if the reverse loop values would be improved if we went around again

            if (temp_chain_record.backward_loops.back() < temp_chain_record.backward_loops.front()) {
                temp_chain_record.backward_loops[0] = temp_chain_record.backward_loops.back();
                size_t node_i = 1;
                size_t last_node_length = 0;
                for (size_t i = 1 ; i < temp_chain_record.children.size()-1 ; i++ ) {
                    auto& child = temp_chain_record.children.at(i);
                    if (child.first == SnarlDistanceIndex::TEMP_SNARL) {
                        SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records.at(child.second);
                        size_t new_loop_distance = SnarlDistanceIndex::sum({temp_chain_record.backward_loops.at(node_i-1), 2*temp_snarl_record.min_length, 2*temp_snarl_record.start_node_length}); 
                        if (temp_chain_record.chain_components.at(node_i)!= 0 || new_loop_distance >= temp_chain_record.backward_loops.at(node_i)) {
                            //If this is a new chain component or it doesn't improve, stop
                            break;
                        } else {
                            //otherwise record the better distance
                            temp_chain_record.backward_loops.at(node_i) = new_loop_distance;

                        }
                        node_i++;
                        last_node_length = 0;
                    } else {
                        if (last_node_length != 0) {
                            size_t new_loop_distance = SnarlDistanceIndex::sum({temp_chain_record.backward_loops.at(node_i-1), 
                                    2*last_node_length}); 
                            size_t old_loop_distance = temp_chain_record.backward_loops.at(node_i);
                            temp_chain_record.backward_loops.at(node_i) = std::min(old_loop_distance,new_loop_distance);
                            node_i++;
                        }
                        last_node_length = temp_index.temp_node_records.at(child.second - temp_index.min_node_id).node_length;
                    }
                }
            }
            if (temp_chain_record.forward_loops.front() < temp_chain_record.forward_loops.back()) {
                //If this is a looping chain and looping improves the forward loops, 
                //then we have to keep going around to update distance

                temp_chain_record.forward_loops.back() = temp_chain_record.forward_loops.front();
                size_t last_node_length = 0;
                node_i = temp_chain_record.prefix_sum.size() - 2;
                for (int j = (int)temp_chain_record.children.size() - 1 ; j >= 0 ; j--) {
                    auto& child = temp_chain_record.children.at(j);
                    if (child.first == SnarlDistanceIndex::TEMP_SNARL){
                        SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records.at(child.second);
                        size_t new_distance = SnarlDistanceIndex::sum({temp_chain_record.forward_loops.at(node_i+1) , 2* temp_snarl_record.min_length,
                                              2*temp_snarl_record.end_node_length});
                        if (temp_chain_record.chain_components.at(node_i) != temp_chain_record.chain_components.at(node_i+1) ||
                            new_distance >= temp_chain_record.forward_loops.at(node_i)){
                            //If this is a new component or the distance doesn't improve, stop looking
                            break;
                        } else {
                            //otherwise, update the distance
                            temp_chain_record.forward_loops.at(node_i) = new_distance;
                        }
                        node_i --;
                        last_node_length =0;
                    } else {
                        if (last_node_length != 0) {
                            size_t new_distance = SnarlDistanceIndex::sum({temp_chain_record.forward_loops.at(node_i+1) , 2* last_node_length});
                            size_t old_distance = temp_chain_record.forward_loops.at(node_i);
                            temp_chain_record.forward_loops.at(node_i) = std::min(old_distance, new_distance);
                            node_i--;
                        }
                        last_node_length = temp_index.temp_node_records.at(child.second - temp_index.min_node_id).node_length;
                    }
                } 
            }
        }

        temp_index.max_distance = std::max(temp_index.max_distance, temp_chain_record.prefix_sum.back());
        temp_index.max_distance = temp_chain_record.forward_loops.back() == std::numeric_limits<size_t>::max() ? temp_index.max_distance : std::max(temp_index.max_distance, temp_chain_record.forward_loops.back());
        temp_index.max_distance = temp_chain_record.backward_loops.front() == std::numeric_limits<size_t>::max() ? temp_index.max_distance : std::max(temp_index.max_distance, temp_chain_record.backward_loops.front());

    }

#ifdef debug_distance_indexing
    cerr << "Filling in the distances in root snarls" << endl;
#endif
    for (pair<SnarlDistanceIndex::temp_record_t, size_t>& component_index : temp_index.components) {
        if (component_index.first == SnarlDistanceIndex::TEMP_SNARL) {
            SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records.at(component_index.second);
            populate_snarl_index(temp_index, component_index, size_limit, graph);
            temp_snarl_record.min_length = std::numeric_limits<size_t>::max();//TODO: This is true but might be better to store it as something else so we can bit compress later
        }
    }
    temp_index.root_structure_count = temp_index.components.size();
    assert(temp_index.components.size() == temp_index.root_structure_count);
#ifdef debug_distance_indexing
    cerr << "Finished temp index with " << temp_index.root_structure_count << " connected components" << endl;
#endif
    return temp_index;
}



/*Fill in the snarl index.
 * The index will already know its boundaries and everything knows their relationships in the
 * snarl tree. This needs to fill in the distances and the ranks of children in the snarl
 * The rank of a child is arbitrary, except that the start node will always be 0 and the end node
 * will always be the node count+1 (since node count doesn't count the boundary nodes)
 */
void populate_snarl_index(
                SnarlDistanceIndex::TemporaryDistanceIndex& temp_index,
                pair<SnarlDistanceIndex::temp_record_t, size_t> snarl_index, size_t size_limit, const HandleGraph* graph) {
#ifdef debug_distance_indexing
    cerr << "Getting the distances for snarl " << temp_index.structure_start_end_as_string(snarl_index) << endl;
    assert(snarl_index.first == SnarlDistanceIndex::TEMP_SNARL);
#endif
    unordered_map<pair<pair<size_t, bool>, pair<size_t, bool>>, size_t> temp_snarl_distances;
    SnarlDistanceIndex::TemporaryDistanceIndex::TemporarySnarlRecord& temp_snarl_record = temp_index.temp_snarl_records.at(snarl_index.second);
    temp_snarl_record.is_simple=true;




    /*Helper function to find the ancestor of a node that is a child of this snarl */
    auto get_ancestor_of_node = [&](pair<SnarlDistanceIndex::temp_record_t, size_t> curr_index) {

        //This is a child that isn't a node, so it must be a chain
        if (curr_index.second == temp_snarl_record.start_node_id || 
            curr_index.second == temp_snarl_record.end_node_id) {
            return curr_index;
        }

        //Otherwise, walk up until we hit the current snarl
        pair<SnarlDistanceIndex::temp_record_t, size_t> parent_index = temp_index.temp_node_records.at(curr_index.second-temp_index.min_node_id).parent;
        while (parent_index != snarl_index) {
            curr_index=parent_index;
            parent_index = parent_index.first == SnarlDistanceIndex::TEMP_SNARL ? temp_index.temp_snarl_records.at(parent_index.second).parent
                                                            : temp_index.temp_chain_records.at(parent_index.second).parent;
#ifdef debug_distance_indexing
            assert(parent_index.first != SnarlDistanceIndex::TEMP_ROOT); 
#endif
        }
        
        return curr_index;
    };


    /*Now go through each of the children and add distances from that child to everything reachable from it
     * Start a dijkstra traversal from each node side in the snarl and record all distances
     */

    //Add the start and end nodes to the list of children so that we include them in the traversal 
    //TODO: Copying the list
    vector<pair<SnarlDistanceIndex::temp_record_t, size_t>> all_children = temp_snarl_record.children;
    if (!temp_snarl_record.is_root_snarl) {


        all_children.emplace_back(SnarlDistanceIndex::TEMP_NODE, temp_snarl_record.start_node_id);
        all_children.emplace_back(SnarlDistanceIndex::TEMP_NODE, temp_snarl_record.end_node_id);
    }

    while (!all_children.empty()) {
        const pair<SnarlDistanceIndex::temp_record_t, size_t> start_index = std::move(all_children.back());
        all_children.pop_back();

        bool is_internal_node = false;

        //Check if this node is a tip
        if ((start_index.first == SnarlDistanceIndex::TEMP_NODE 
             && start_index.second != temp_snarl_record.start_node_id 
             && start_index.second != temp_snarl_record.end_node_id) 
            || 
            (start_index.first == SnarlDistanceIndex::TEMP_CHAIN && temp_index.temp_chain_records.at(start_index.second).is_trivial)) {
            //If this is an internal node
            is_internal_node = true;
            nid_t node_id = start_index.first == SnarlDistanceIndex::TEMP_NODE ? start_index.second : temp_index.temp_chain_records.at(start_index.second).start_node_id;
            size_t rank = start_index.first == SnarlDistanceIndex::TEMP_NODE ? temp_index.temp_node_records.at(start_index.second-temp_index.min_node_id).rank_in_parent 
                                                          : temp_index.temp_chain_records.at(start_index.second).rank_in_parent;
            
            bool has_edges = false;
            graph->follow_edges(graph->get_handle(node_id, false), false, [&](const handle_t next_handle) {
                has_edges = true;
            });
            if (!has_edges) {
                temp_index.temp_node_records.at(node_id-temp_index.min_node_id).is_tip = true;
                temp_snarl_record.tippy_child_ranks.insert(rank);
                temp_snarl_record.is_simple=false; //It is a tip so this isn't simple snarl
            }
            has_edges = false;
            graph->follow_edges(graph->get_handle(node_id, true), false, [&](const handle_t next_handle) {
                has_edges = true;
            });
            if (!has_edges) {
                temp_index.temp_node_records.at(node_id-temp_index.min_node_id).is_tip = true;
                temp_snarl_record.tippy_child_ranks.insert(rank);
                temp_snarl_record.is_simple=false; //It is a tip so this isn't simple snarl
            }
        } else if (start_index.first == SnarlDistanceIndex::TEMP_CHAIN && !temp_index.temp_chain_records.at(start_index.second).is_trivial) {
            //If this is an internal chain, then it isn't a simple snarl
            temp_snarl_record.is_simple=false;
        }

        bool start_is_tip = start_index.first == SnarlDistanceIndex::TEMP_NODE 
                      ? temp_index.temp_node_records.at(start_index.second-temp_index.min_node_id).is_tip 
                      : temp_index.temp_chain_records.at(start_index.second).is_tip;

        size_t start_rank = start_index.first == SnarlDistanceIndex::TEMP_NODE 
                ? temp_index.temp_node_records.at(start_index.second-temp_index.min_node_id).rank_in_parent
                : temp_index.temp_chain_records.at(start_index.second).rank_in_parent;


        if (start_index.first == SnarlDistanceIndex::TEMP_NODE && start_index.second == temp_snarl_record.start_node_id) {
            start_rank = 0;
        } else if (start_index.first == SnarlDistanceIndex::TEMP_NODE && start_index.second == temp_snarl_record.end_node_id) {
            start_rank = 1;
        } //TODO:
          //else {
          //  assert(start_rank != 0 && start_rank != 1);
          //}

        if ( (temp_snarl_record.node_count < size_limit || size_limit == 0) && !start_is_tip &&
             !start_rank == 0 && ! start_rank == 1) {
            //If we don't care about internal distances, and we also are not at a boundary or tip
            continue;
        }

        //Start from either direction for all nodes, but only going in for start and end
        vector<bool> directions;
        if (start_index.first == SnarlDistanceIndex::TEMP_NODE && start_index.second == temp_snarl_record.start_node_id) {
            directions.emplace_back(temp_snarl_record.start_node_rev);
        } else if (start_index.first == SnarlDistanceIndex::TEMP_NODE && start_index.second == temp_snarl_record.end_node_id){
            directions.emplace_back(!temp_snarl_record.end_node_rev);
        } else {
            directions.emplace_back(true);
            directions.emplace_back(false);
        }
        for (bool start_rev : directions) {
            //Start a dijkstra traversal from start_index going in the direction indicated by start_rev
            //Record the distances to each node (child of the snarl) found
            size_t reachable_node_count = 0; //How many nodes can we reach from this node side?

#ifdef debug_distance_indexing
            cerr << "  Starting from child " << temp_index.structure_start_end_as_string(start_index)
                 << " going " << (start_rev ? "rev" : "fd") << endl;
#endif

            //Define a NetgraphNode as the value for the priority queue:
            // <distance, <<type of node, index into temp_node/chain_records>, direction>
            using NetgraphNode = pair<size_t, pair<pair<SnarlDistanceIndex::temp_record_t, size_t>, bool>>; 
            auto cmp = [] (const NetgraphNode a, const NetgraphNode b) {
                return a.first > b.first;
            };
            std::priority_queue<NetgraphNode, vector<NetgraphNode>, decltype(cmp)> queue(cmp);
            unordered_set<pair<pair<SnarlDistanceIndex::temp_record_t, size_t>, bool>> seen_nodes;
            queue.push(make_pair(0, make_pair(start_index, start_rev)));

            while (!queue.empty()) {

                size_t current_distance = queue.top().first;
                pair<SnarlDistanceIndex::temp_record_t, size_t> current_index = queue.top().second.first;
                bool current_rev = queue.top().second.second;
                seen_nodes.emplace(queue.top().second);
                queue.pop();

                //The handle that we need to follow to get the next reachable nodes
                //If the current node is a node, then its just the node. Otherwise, it's the 
                //opposite side of the child chain
                handle_t current_end_handle = current_index.first == SnarlDistanceIndex::TEMP_NODE ? 
                        graph->get_handle(current_index.second, current_rev) :
                        (current_rev ? graph->get_handle(temp_index.temp_chain_records[current_index.second].start_node_id, 
                                                        !temp_index.temp_chain_records[current_index.second].start_node_rev) 
                                  : graph->get_handle(temp_index.temp_chain_records[current_index.second].end_node_id, 
                                                      temp_index.temp_chain_records[current_index.second].end_node_rev));

#ifdef debug_distance_indexing
                        cerr << "    at child " << temp_index.structure_start_end_as_string(current_index) << " going "
                             << (current_rev ? "rev" : "fd") << " at actual node " << graph->get_id(current_end_handle) 
                             << (graph->get_is_reverse(current_end_handle) ? "rev" : "fd") << endl;
#endif
                graph->follow_edges(current_end_handle, false, [&](const handle_t next_handle) {
                    if (graph->get_id(current_end_handle) == graph->get_id(next_handle)){
                        //If there are any loops then this isn't a simple snarl
                        temp_snarl_record.is_simple = false;
                    }

                    reachable_node_count++;
                    //At each of the nodes reachable from the current one, fill in the distance from the start
                    //node to the next node (current_distance). If this handle isn't leaving the snarl,
                    //add the next nodes along with the distance to the end of the next node
                    auto& node_record = temp_index.temp_node_records.at(graph->get_id(next_handle)-temp_index.min_node_id);

                    //The index of the snarl's child that next_handle represents
                    pair<SnarlDistanceIndex::temp_record_t, size_t> next_index = get_ancestor_of_node(make_pair(SnarlDistanceIndex::TEMP_NODE, graph->get_id(next_handle))); 

                    bool next_is_tip = start_index.first == SnarlDistanceIndex::TEMP_NODE 
                              ? temp_index.temp_node_records.at(start_index.second-temp_index.min_node_id).is_tip 
                              : temp_index.temp_chain_records.at(start_index.second).is_tip;

                    //The rank and orientation of next in the snarl
                    size_t next_rank = next_index.first == SnarlDistanceIndex::TEMP_NODE 
                            ? node_record.rank_in_parent
                            : temp_index.temp_chain_records[next_index.second].rank_in_parent;
                    if (next_index.first == SnarlDistanceIndex::TEMP_NODE && next_index.second == temp_snarl_record.start_node_id) {
                        next_rank = 0;
                    } else if (next_index.first == SnarlDistanceIndex::TEMP_NODE && next_index.second == temp_snarl_record.end_node_id) {
                        next_rank = 1;
                    } else {
                        //If the next thing wasn't a boundary node and this was an internal node, then it isn't a simple snarl
                        if (is_internal_node) {
                            temp_snarl_record.is_simple = false;
                        }
                    }//TODO: This won't be true of root snarls 
                      //else {
                      //  assert(next_rank != 0 && next_rank != 1);
                      //}
                    bool next_rev = next_index.first == SnarlDistanceIndex::TEMP_NODE || temp_index.temp_chain_records[next_index.second].is_trivial 
                            ? graph->get_is_reverse(next_handle) 
                            : graph->get_id(next_handle) == temp_index.temp_chain_records[next_index.second].end_node_id;

                    if (size_limit != 0 &&
                        (temp_snarl_record.node_count < size_limit ||
                         (start_rank == 0 || start_rank == 1 || next_rank == 0 || next_rank == 1))) {
                        //If we are looking at all distances or we are looking at tips or boundaries

                        //Set the distance
                        pair<size_t, bool> start = !temp_snarl_record.is_root_snarl && (start_rank == 0 || start_rank == 1) 
                            ? make_pair(start_rank, false) : make_pair(start_rank, !start_rev);
                        pair<size_t, bool> next = !temp_snarl_record.is_root_snarl && (next_rank == 0 || next_rank == 1) 
                            ? make_pair(next_rank, false) : make_pair(next_rank, next_rev);
                        if (!temp_snarl_distances.count(make_pair(start, next)) ) {

                            temp_snarl_distances[make_pair(start, next)] = current_distance;
                            temp_snarl_record.max_distance = std::max(temp_snarl_record.max_distance, current_distance);
#ifdef debug_distance_indexing
                            cerr << "           Adding distance between ranks " << start.first << " " << start.second << " and " << next.first << " " << next.second << ": " << current_distance << endl;
#endif
                        }
                    }


                    if (seen_nodes.count(make_pair(next_index, next_rev)) == 0 &&
                        graph->get_id(next_handle) != temp_snarl_record.start_node_id &&
                        graph->get_id(next_handle) != temp_snarl_record.end_node_id) {
                        //If this isn't leaving the snarl, then add the next node to the queue, 
                        //along with the distance to traverse it
                        size_t next_node_len = next_index.first == SnarlDistanceIndex::TEMP_NODE ? graph->get_length(next_handle) :
                                        temp_index.temp_chain_records[next_index.second].min_length;
                        queue.push(make_pair(current_distance + next_node_len, 
                                       make_pair(next_index, next_rev)));
                    }
                    if (next_index.first == SnarlDistanceIndex::TEMP_CHAIN) {
                        size_t loop_distance = next_rev ? temp_index.temp_chain_records[next_index.second].backward_loops.back() 
                                                         : temp_index.temp_chain_records[next_index.second].forward_loops.front();
                        if (loop_distance != std::numeric_limits<size_t>::max() &&
                            seen_nodes.count(make_pair(next_index, !next_rev)) == 0 &&
                            graph->get_id(next_handle) != temp_snarl_record.start_node_id &&
                            graph->get_id(next_handle) != temp_snarl_record.end_node_id) {
                            //If the next node can loop back on itself, then add the next node in the opposite direction
                            size_t next_node_len = loop_distance + 2 * graph->get_length(next_handle);
                            queue.push(make_pair(current_distance + next_node_len, 
                                           make_pair(next_index, !next_rev)));
                        }
                    }
#ifdef debug_distance_indexing
                    cerr << "        reached child " << temp_index.structure_start_end_as_string(next_index) << "going " 
                         << (next_rev ? "rev" : "fd") << " with distance " << current_distance << " for ranks " << start_rank << " " << next_rank << endl;
#endif
                });
            }
            if (is_internal_node && reachable_node_count != 1) {
                //If this is an internal node, then it must have only one edge for it to be a simple snarl
                temp_snarl_record.is_simple = false;
            }
        }
        if (start_rank != 0 && start_rank != 1) {

            size_t child_max_length = start_index.first == SnarlDistanceIndex::TEMP_NODE 
                ? temp_index.temp_node_records.at(start_index.second-temp_index.min_node_id).node_length
                : temp_index.temp_chain_records.at(start_index.second).max_length;
            //The distance through the whole snarl traversing this node forwards
            //(This might actually be traversing it backwards but it doesn't really matter)
            pair<size_t, bool> start_in = make_pair(0, false);
            pair<size_t, bool> end_in = make_pair(1, false); 

            size_t dist_start_left = temp_snarl_distances.count(make_pair(make_pair(start_rank, false), start_in)) 
                    ? temp_snarl_distances.at(make_pair(make_pair(start_rank, false), start_in)) 
                    :std::numeric_limits<size_t>::max();
            size_t dist_end_right = temp_snarl_distances.count(make_pair(make_pair(start_rank, true), end_in)) 
                    ? temp_snarl_distances.at(make_pair(make_pair(start_rank, true), end_in))
                    : std::numeric_limits<size_t>::max();
            size_t dist_start_right = temp_snarl_distances.count(make_pair(make_pair(start_rank, true), start_in)) 
                    ? temp_snarl_distances.at(make_pair(make_pair(start_rank, true), start_in))
                    : std::numeric_limits<size_t>::max();
            size_t dist_end_left = temp_snarl_distances.count(make_pair(make_pair(start_rank, false), end_in))
                    ? temp_snarl_distances.at(make_pair(make_pair(start_rank, false), end_in))
                    : std::numeric_limits<size_t>::max();

            size_t snarl_length_fd = SnarlDistanceIndex::sum({
                    dist_start_left, dist_end_right,child_max_length});
            //The same thing traversing this node backwards
            size_t snarl_length_rev = SnarlDistanceIndex::sum({
                    dist_start_right, dist_end_left, child_max_length});
            //The max that isn't infinite
            size_t max_length = 
                snarl_length_rev == std::numeric_limits<size_t>::max() 
                ? snarl_length_fd 
                : (snarl_length_fd == std::numeric_limits<size_t>::max() 
                        ? snarl_length_rev 
                        : std::max(snarl_length_rev, snarl_length_fd));
            if (max_length != std::numeric_limits<size_t>::max()) {
                temp_snarl_record.max_length = std::max(temp_snarl_record.max_length, max_length);
            }
        }
    }

    //If this is a simple snarl (one with only single nodes that connect to the start and end nodes), then
    // we want to remember if the child nodes are reversed 
    if (temp_snarl_record.is_simple) {
        for (size_t i = 0 ; i < temp_snarl_record.node_count ; i++) {
            //Get the index of the child
            const pair<SnarlDistanceIndex::temp_record_t, size_t>& child_index = temp_snarl_record.children[i];
            //Which is a node
            assert(child_index.first == SnarlDistanceIndex::TEMP_NODE);

            //And get the record
            SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord& temp_node_record =
                 temp_index.temp_node_records[child_index.second-temp_index.min_node_id];
            size_t rank =temp_node_record.rank_in_parent;

            //See if it is reversed in the parent by checking if it reaches the boundaries forwards or backwards
            bool reaches_node_end_to_start = temp_snarl_distances.count(
                std::make_pair(std::make_pair(rank, true),
                               std::make_pair(0, false))) != 0;
            bool reaches_start_to_node_end = temp_snarl_distances.count(
                std::make_pair(std::make_pair(0, false),
                               std::make_pair(rank, true))) != 0;
            
            //Set the orientation of this node in the simple snarl
            temp_node_record.reversed_in_parent = reaches_node_end_to_start || reaches_start_to_node_end;

        }
    }

    //Now that the distances are filled in, predict the size of the snarl in the index
    temp_index.max_index_size += temp_snarl_record.get_max_record_length();
    if (temp_snarl_record.is_simple) {
        temp_index.max_index_size -= (temp_snarl_record.children.size() * SnarlDistanceIndex::TemporaryDistanceIndex::TemporaryNodeRecord::get_max_record_length());
    }

    //Get the minimum length of the snarl
    if (temp_snarl_distances.count(make_pair(make_pair(0, false), make_pair(1, false)))){
        temp_snarl_record.min_length = temp_snarl_distances.at(make_pair(make_pair(0, false), make_pair(1, false)));
    } else if (temp_snarl_distances.count(make_pair(make_pair(1, false), make_pair(0, false)))){
        temp_snarl_record.min_length = temp_snarl_distances.at(make_pair(make_pair(1, false), make_pair(0, false)));
    } else {
        //The snarl is not start-end connected
        temp_snarl_record.min_length = std::numeric_limits<size_t>::max();
    }

    //Get the loop distances for the snarl
    temp_snarl_record.loop_start =
        temp_snarl_distances.count(make_pair(make_pair(0, false), make_pair(0, false)))
      ? temp_snarl_distances.at(make_pair(make_pair(0, false), make_pair(0, false)))
      : std::numeric_limits<size_t>::max();
    temp_snarl_record.loop_end =
        temp_snarl_distances.count(make_pair(make_pair(1, false), make_pair(1, false)))
      ? temp_snarl_distances.at(make_pair(make_pair(1, false), make_pair(1, false)))
      : std::numeric_limits<size_t>::max();


    //Record the distances in the snarl
    for (auto& distances : temp_snarl_distances) {
        temp_snarl_record.distances.emplace_back(distances.first.first, distances.first.second, distances.second);
    }
}


//Given an alignment to a graph and a range, find the set of nodes in the
//graph for which the minimum distance from the position to any position
//in the node is within the given distance range
//If look_forward is true, then start from the start of the path forward,
//otherwise start from the end going backward
void subgraph_in_distance_range(const SnarlDistanceIndex& distance_index, const Path& path, const HandleGraph* super_graph, size_t min_distance,
                                        size_t max_distance, std::unordered_set<nid_t>& subgraph, bool look_forward){

    //The position we're starting from - either the start or end of the path
    pos_t start_pos;
    size_t node_len;
    if (look_forward ){
        start_pos = initial_position(path);
        node_len = super_graph->get_length(super_graph->get_handle(get_id(start_pos)));
    } else {
        start_pos = final_position(path);
        node_len = super_graph->get_length(super_graph->get_handle(get_id(start_pos)));
        start_pos = reverse_base_pos(start_pos, node_len);
    }

#ifdef debug_subgraph
cerr << "Start positon: "<< start_pos << endl;
#endif
    //The distance from the position to the ends of the current node(/snarl/chain)
    size_t current_distance_left = is_rev(start_pos) ? node_len - get_offset(start_pos) : std::numeric_limits<size_t>::max() ;
    size_t current_distance_right = is_rev(start_pos) ? std::numeric_limits<size_t>::max() : node_len - get_offset(start_pos) ;

    //Graph node of the start and end of the current node(/snarl/chain) pointing out
    net_handle_t current_net = distance_index.get_node_net_handle(get_id(start_pos));
    net_handle_t parent = distance_index.get_parent(current_net);

    //The id and orientation of nodes that are too close and should be avoided
    hash_set<pair<id_t, bool>> seen_nodes;
    //Nodes that we want to start a search from - the distance is smaller or equal to than min_distance but
    //we can't walk out any further along the snarl tree without exceeding it
    //The distance is the distance from the start position to the beginning (or end if its backwards) of the node,
    //including the position
    vector<pair<handle_t, size_t>> search_start_nodes;

    if ((current_distance_left != std::numeric_limits<size_t>::max() && current_distance_left >= min_distance) ||
           (current_distance_right != std::numeric_limits<size_t>::max() && current_distance_right >= min_distance)) {
        //If the distance to either end of the node is within the range

        //Add this node to the subgraph
        subgraph.emplace(get_id(start_pos));

        handle_t start = is_rev(start_pos) ? distance_index.get_handle(distance_index.flip(current_net), super_graph)
                                           : distance_index.get_handle(current_net, super_graph); 

        //Add any node one step out from this one to search_start_nodes
        super_graph->follow_edges(start, 
                false, [&](const handle_t& next_handle) {
            search_start_nodes.emplace_back(next_handle,current_distance_right);
        });

        //Search for reachable nodes
        add_nodes_in_distance_range(super_graph, min_distance, max_distance, subgraph, search_start_nodes, seen_nodes); 

        return;
    }


    //helper function to walk along a chain from the current node until the distance traversed
    //exceeds the minimum limit. Add the node just before this happens to search_start_nodes
    auto add_start_node_from_chain = [&] (net_handle_t current_node, size_t& current_distance){
#ifdef debug_subgraph
        cerr << "Walk along parent chain " << distance_index.net_handle_as_string(distance_index.get_parent(current_node)) << " from " << distance_index.net_handle_as_string(current_node) << " with " << current_distance << endl;
#endif
        if (distance_index.is_trivial_chain(distance_index.get_parent(current_node))){
            cerr << "Trivial cahin" << endl;
            return;
        }
        bool finished_chain = false;
        while (current_distance <= min_distance && !finished_chain) {
            finished_chain = distance_index.follow_net_edges(current_node, super_graph, false, 
                [&](const net_handle_t& next) {
                    size_t next_length = distance_index.minimum_length(next);
                    //If the next child is a snarl, then the distance to loop in the snarl
                    size_t next_loop = std::numeric_limits<size_t>::max();
                    if (distance_index.is_snarl(next)) {
                        net_handle_t bound_fd = distance_index.get_bound(next, distance_index.ends_at(next) == SnarlDistanceIndex::START, true);
                        next_loop = distance_index.distance_in_parent(next, bound_fd, bound_fd, super_graph, max_distance);
                    }
                    size_t next_max_length = distance_index.maximum_length(next);
#ifdef debug_subgraph
                    cerr << "\tnext node: " << distance_index.net_handle_as_string(next) << " with distance " << current_distance << " and min and max lengths " << next_length << " " << next_max_length  << " and loop value " << next_loop << endl;
#endif
                    if (( SnarlDistanceIndex::sum({next_max_length, current_distance}) != std::numeric_limits<size_t>::max()  &&
                         SnarlDistanceIndex::sum({next_max_length, current_distance}) >= min_distance) ||
                         next_loop != std::numeric_limits<size_t>::max()){
                        if (distance_index.is_node(next)) {
                            size_t curr_distance_end = SnarlDistanceIndex::sum({next_max_length, current_distance});
                            //If its a node that puts us over, add the node to the subgraph, then start the search from that node
                            if ((current_distance >= min_distance && current_distance < max_distance) ||
                                 (curr_distance_end >= min_distance && curr_distance_end <= max_distance) ||
                                 (current_distance < min_distance && curr_distance_end >= max_distance) ||
                                 next_loop != std::numeric_limits<size_t>::max()) {
                                subgraph.emplace(distance_index.node_id(next));
                            }
                            super_graph->follow_edges(distance_index.get_handle(next, super_graph), false, [&](const handle_t& next_handle) {
                                search_start_nodes.emplace_back(next_handle, 
                                    SnarlDistanceIndex::sum({current_distance, next_length}));
                            });
                        } else {
                            //If it's a snarl, then we'll start from the last node
#ifdef debug_subgraph
                            cerr << "\t\tAdding node from a chain " << distance_index.net_handle_as_string(next) << " with distance " << current_distance << endl;
#endif
                            super_graph->follow_edges(distance_index.get_handle(current_node, super_graph), false, [&](const handle_t& next_handle) {
                                search_start_nodes.emplace_back(next_handle,current_distance);
                            });
                        }
                        //If we added something, stop traversing the chain
                        return true;
                    } else if (distance_index.is_node(next)) {
                        seen_nodes.emplace(distance_index.node_id(next), distance_index.ends_at(next) == SnarlDistanceIndex::END);
                    }
                    current_node = next;
                    current_distance = SnarlDistanceIndex::sum({next_length, current_distance});
                    if (current_distance > max_distance) {
                        return true;
                    } else {
                        return false;
                    }
            }); 
        }
    };

    while (!distance_index.is_root(parent)) {
#ifdef debug_subgraph
        cerr << "At child " << distance_index.net_handle_as_string(current_net) << " with distances " << current_distance_left << " " << current_distance_right << endl;
#endif

        //Distance from the start of the parent to the left of the current node
        size_t max_parent_length = distance_index.maximum_length(parent);

        //Distances to loop in the chain  (or inf if parent isn't a chain
        size_t distance_loop_right = distance_index.is_chain(parent) 
            ? distance_index.distance_in_parent(parent, current_net, current_net, super_graph, max_distance)
            : std::numeric_limits<size_t>::max();
        size_t distance_loop_left = distance_index.is_chain(parent) 
            ? distance_index.distance_in_parent(parent, distance_index.flip(current_net), distance_index.flip(current_net), super_graph, max_distance)
            : std::numeric_limits<size_t>::max();

        //Distances to get to the ends of the parent
        size_t distance_start_left = SnarlDistanceIndex::sum({current_distance_left,
                    distance_index.distance_to_parent_bound(parent, true, current_net, true)});
        size_t distance_start_right = SnarlDistanceIndex::sum({current_distance_right,
                     distance_index.distance_to_parent_bound(parent, true, current_net, false)});
        size_t distance_end_left = SnarlDistanceIndex::sum({current_distance_left,
                    distance_index.distance_to_parent_bound(parent, false, current_net, true)});
        size_t distance_end_right = SnarlDistanceIndex::sum({current_distance_right,
                     distance_index.distance_to_parent_bound(parent, false, current_net, false)});

        if ((SnarlDistanceIndex::sum({max_parent_length, current_distance_left}) != std::numeric_limits<size_t>::max() &&
            SnarlDistanceIndex::sum({max_parent_length, current_distance_left}) >= min_distance) 
            || 
            (SnarlDistanceIndex::sum({distance_loop_left, current_distance_left}) != std::numeric_limits<size_t>::max() &&
             SnarlDistanceIndex::sum({distance_loop_left, current_distance_left}) >= min_distance)) {
            //if the distance walking out from the start of the current node will exceed the minimum 
            //distance within the parent
            //If the parent is a snarl then add the start of the current node to the list of things to search 
            //from, if it's a chain then walk along the chain until the distance exceeds it
            if (distance_index.is_snarl(parent)){
#ifdef debug_subgraph
                cerr << "Adding the child " << distance_index.net_handle_as_string(parent) << ": " << distance_index.net_handle_as_string(distance_index.get_bound(current_net, false, false)) << " with distance left " << current_distance_left << endl;
#endif
                super_graph->follow_edges(distance_index.get_handle(distance_index.get_bound(current_net, false, false), super_graph), 
                        false, [&](const handle_t& next_handle) {
                    search_start_nodes.emplace_back(next_handle,current_distance_left);
                });
            } else {
                //current_distance_left is the distance including current_node
                //When adding the length of the next node exceeds the min_distance,
                //add the current node
                add_start_node_from_chain(distance_index.flip(current_net), current_distance_left);
            }
        }
        if ((SnarlDistanceIndex::sum({max_parent_length, current_distance_right}) != std::numeric_limits<size_t>::max() &&
            SnarlDistanceIndex::sum({max_parent_length, current_distance_right}) >= min_distance)
            ||
            (SnarlDistanceIndex::sum({distance_loop_right, current_distance_right}) != std::numeric_limits<size_t>::max() &&
            SnarlDistanceIndex::sum({distance_loop_right, current_distance_right}) >= min_distance)) {
            //The same thing for the end of the current node
            if (distance_index.is_snarl(parent)){
#ifdef debug_subgraph
                cerr << "Adding the child of " << distance_index.net_handle_as_string(parent) << ": "<< distance_index.net_handle_as_string(distance_index.get_bound(current_net, false, false)) << " with distance " << current_distance_right << endl;
#endif
                super_graph->follow_edges(distance_index.get_handle(distance_index.get_bound(current_net, true, false), super_graph), 
                        false, [&](const handle_t& next_handle) {
                    search_start_nodes.emplace_back(next_handle,current_distance_right);
                });
            } else {
                //current_distance_right is the distance including current_node
                //When adding the length of the next node exceeds the min_distance,
                //add the current node
                add_start_node_from_chain(current_net, current_distance_right);
            }
        }
        current_distance_left = std::min(distance_start_left, distance_start_right);
        current_distance_right = std::min(distance_end_left, distance_end_right);

        current_net = std::move(parent);
        parent = distance_index.get_parent(current_net);
    }
    if (current_distance_left <= min_distance) {
#ifdef debug_subgraph
        cerr << "Adding the end of a child of the root " << distance_index.net_handle_as_string(distance_index.get_bound(current_net, false, false)) << " with distance " << current_distance_left << endl;
#endif

        search_start_nodes.emplace_back(distance_index.get_handle(
                distance_index.get_bound(current_net, false, false), super_graph),
            current_distance_left);
    }
    if (current_distance_right <= min_distance) {
#ifdef debug_subgraph
        cerr << "Adding the end of a child of the root " << distance_index.net_handle_as_string(distance_index.get_bound(current_net, false, false)) << " with distance " << current_distance_right << endl;
#endif
        search_start_nodes.emplace_back(distance_index.get_handle(
                distance_index.get_bound(current_net, true, false), super_graph),
            current_distance_right);
    }
    add_nodes_in_distance_range(super_graph, min_distance, max_distance, subgraph, search_start_nodes, seen_nodes); 

    return;
}


///Helper for subgraph_in_distance_range
///Given starting handles in the super graph and the distances to each handle (including the start position and
//the first position in the handle), add all nodes within the distance range, excluding nodes in seen_nodes
void add_nodes_in_distance_range(const HandleGraph* super_graph, size_t min_distance, size_t max_distance,
                        std::unordered_set<nid_t>& subgraph, vector<pair<handle_t, size_t>>& start_nodes,
                        hash_set<pair<nid_t, bool>>& seen_nodes) {
#ifdef debug_subgraph
    cerr << "Starting search from nodes " << endl;
    for (auto& start_handle : start_nodes) {
        cerr << "\t" << super_graph->get_id(start_handle.first) << " " << super_graph->get_is_reverse(start_handle.first)
             << " with distance " << start_handle.second << endl;
    }
#endif

    //Order based on the distance to the position (handle)
    auto cmp =  [] (const pair<handle_t, size_t> a, const pair<handle_t, size_t> b ) {
            return a.second > b.second;
        };
    priority_queue< pair<handle_t, size_t>, vector<pair<handle_t, size_t>>, decltype(cmp)> next_handles (cmp);
    for (auto& start_handle : start_nodes) {
        next_handles.emplace(start_handle);
    }
    bool first_node = true;

    while (next_handles.size() > 0) {
        //Traverse the graph, adding nodes if they are within the range
        handle_t curr_handle=next_handles.top().first;
        size_t curr_distance=next_handles.top().second;
        next_handles.pop();
#ifdef debug_subgraph
        cerr << "At node " << super_graph->get_id(curr_handle) << " with distance " << curr_distance << endl;
#endif
        if (seen_nodes.count(make_pair(super_graph->get_id(curr_handle), super_graph->get_is_reverse(curr_handle))) == 0) {
            seen_nodes.emplace(super_graph->get_id(curr_handle), super_graph->get_is_reverse(curr_handle));

            size_t node_len = super_graph->get_length(curr_handle);
            size_t curr_distance_end = SnarlDistanceIndex::sum({curr_distance, node_len});
            if ((curr_distance >= min_distance && curr_distance < max_distance) ||
                 (curr_distance_end >= min_distance && curr_distance_end <= max_distance) ||
                 (curr_distance < min_distance && curr_distance_end >= max_distance)) {
#ifdef debug_subgraph
                cerr << "\tadding node " << super_graph->get_id(curr_handle) << " " << super_graph->get_is_reverse(curr_handle) << " with distance "
                     << curr_distance << " and node length " << node_len << endl;
#endif
                subgraph.insert(super_graph->get_id(curr_handle));

            }
#ifdef debug_subgraph
            else {
                cerr << "\tdisregarding node " << super_graph->get_id(curr_handle) << " " << super_graph->get_is_reverse(curr_handle)
                     << " with distance " << curr_distance << " and node length " << node_len << endl;
            }
#endif
            curr_distance = SnarlDistanceIndex::sum({node_len, curr_distance});

            //If the end of this node is still within the range, add the next nodes that are within
            if (SnarlDistanceIndex::minus(curr_distance,1) <= max_distance ) {
                super_graph->follow_edges(curr_handle, false, [&](const handle_t& next) {
                    nid_t next_id = super_graph->get_id(next);
                    if (seen_nodes.count(make_pair(next_id, super_graph->get_is_reverse(next))) == 0) {
                        next_handles.emplace(next, curr_distance);
                    }
                    return true;
                });
            }
            first_node = false;
        }

    }

#ifdef debug_subgraph
    cerr << "Subgraph has nodes: ";
    for (const nid_t& node : subgraph) {
        cerr << node << ", ";
    }
    cerr << endl;
#endif
    return;
}

void subgraph_containing_path_snarls(const SnarlDistanceIndex& distance_index, const HandleGraph* graph, const Path& path, std::unordered_set<nid_t>& subgraph) {
    //Get the start and end of the path
    pos_t start_pos = initial_position(path);
    net_handle_t start_node = distance_index.get_node_net_handle(get_id(start_pos));
    subgraph.insert(get_id(start_pos));

    pos_t end_pos = final_position(path);
    net_handle_t end_node = distance_index.get_node_net_handle(get_id(end_pos));
    subgraph.insert(get_id(end_pos));

    //Get the lowest common ancestor
    pair<net_handle_t, bool> lowest_ancestor_bool = distance_index.lowest_common_ancestor(start_node, end_node);
    net_handle_t common_ancestor = lowest_ancestor_bool.first;
    
    
    if (distance_index.is_snarl(common_ancestor) || common_ancestor == start_node) {
        //If the lowest common ancestor is a snarl, just add the entire snarl

        add_descendants_to_subgraph(distance_index, common_ancestor, subgraph);

    } else if (distance_index.is_chain(common_ancestor)) {

        //Get the ancestors of the nodes that are children of the common ancestor
        net_handle_t ancestor1 = distance_index.get_parent(start_node);
        while (ancestor1 != common_ancestor) {
            start_node = ancestor1;
            ancestor1 = distance_index.get_parent(start_node);
        }
        net_handle_t ancestor2 = distance_index.get_parent(end_node);
        while (ancestor2 != common_ancestor) {
            end_node = ancestor2;
            ancestor2 = distance_index.get_parent(end_node);
        }
        assert(ancestor1 == ancestor2);


        //Walk from one ancestor to the other and add everything in the chain
        net_handle_t current_child = distance_index.canonical(distance_index.is_ordered_in_chain(start_node, end_node) ? start_node : end_node);
        net_handle_t end_child = distance_index.canonical(distance_index.is_ordered_in_chain(start_node, end_node) ? end_node : start_node);
        if (distance_index.is_reversed_in_parent(current_child)) {
            current_child = distance_index.flip(current_child);
        }
        if (distance_index.is_reversed_in_parent(end_child)) {
            end_child = distance_index.flip(end_child);
        }

        add_descendants_to_subgraph(distance_index, current_child, subgraph);
        while (current_child != end_child) {
            cerr << "From " << distance_index.net_handle_as_string(current_child) << " reach " << distance_index.net_handle_as_string(end_child) << endl;
            distance_index.follow_net_edges(current_child, graph, false, [&](const net_handle_t& next) {
                add_descendants_to_subgraph(distance_index, next, subgraph);
                current_child = next;

            });
        }

    }
    
}


//Recursively add all nodes in parent to the subgraph
void add_descendants_to_subgraph(const SnarlDistanceIndex& distance_index, const net_handle_t& parent, std::unordered_set<nid_t>& subgraph) {
    if (distance_index.is_node(parent)) {
        subgraph.insert(distance_index.node_id(parent));
    } else {
        distance_index.for_each_child(parent, [&](const net_handle_t& child) {
            add_descendants_to_subgraph(distance_index, child, subgraph);
        });
    }
}

/*Given a position, return distances that can be stored by a minimizer
 *
 * This stores if it's reversed in its parent, the length of the node, 
 * and the offset of the node record
 *
 */


tuple<size_t, size_t, size_t, size_t, bool> get_minimizer_distances (const SnarlDistanceIndex& distance_index,pos_t pos) {

    net_handle_t node_handle = distance_index.get_node_net_handle(get_id(pos));
    net_handle_t parent_handle = distance_index.get_parent(node_handle);
    bool in_top_level_chain = distance_index.is_chain(parent_handle) &&
                              distance_index.is_root(distance_index.get_parent(parent_handle)) &&
                              !distance_index.is_root_snarl(distance_index.get_parent(parent_handle));

    return make_tuple(distance_index.minimum_length(node_handle),
                      in_top_level_chain ? distance_index.get_connected_component_number(node_handle)
                                         : std::numeric_limits<size_t>::max(),
                      in_top_level_chain ? distance_index.get_prefix_sum_value(node_handle)
                                                                     : std::numeric_limits<size_t>::max(),
                      in_top_level_chain ? distance_index.get_chain_component(node_handle)
                                                                     : std::numeric_limits<size_t>::max(),
                      distance_index.is_reversed_in_parent(node_handle));



//TODO: This used to store top-level node information but I'm changing it to store node info for everything 
/*
//If the position is on a boundary node of a top level chain, then return true, 
//the offset of the parent chain, and the offset of the node in the chain
//The second bool will be false and the remaining size_t's will be 0
//
//If the position is on a child node of a top-level simple bubble (bubble has no children and nodes connect only to boundaries)
//return false, 0, 0, true, and the rank of the bubble in its chain, the length of the start
//node of the snarl, the length of the end node (relative to a fd traversal of the chain), and
//the length of the node
//
//If the position is not on a root node (that is, a boundary node of a snarl in a root chain), returns
//false and MIPayload::NO_VALUE for all values
*/
//
//    net_handle_t node_handle = distance_index.get_node_net_handle(get_id(pos));
//    net_handle_t parent_handle = distance_index.get_parent(node_handle);
//
//    if (distance_index.is_root(parent_handle)) {
//        //If this node is a child of the root, then we don't want to cache values
//        return get_empty_minimizer_distances();
//
//    } else if (distance_index.is_chain(parent_handle) && !distance_index.is_trivial_chain(node_handle)) {
//        if (!distance_index.is_snarl(distance_index.get_parent(parent_handle)) 
//            && distance_index.is_root(distance_index.get_parent(parent_handle))) {
//            
//            return tuple<bool, size_t, size_t, bool, size_t, size_t, size_t, size_t, bool>(
//                true, distance_index.get_record_offset(parent_handle), distance_index.get_record_offset_in_chain(node_handle),
//                false, MIPayload::NO_VALUE, MIPayload::NO_VALUE, MIPayload::NO_VALUE, MIPayload::NO_VALUE, false);
//        } else {
//            //If the parent is a nested chain
//            return get_empty_minimizer_distances();
//        }
//    } else if (distance_index.is_snarl(parent_handle)) {
//        //TODO: Add the snrl version later
//        return get_empty_minimizer_distances();
//    } else {
//        throw runtime_error("error: parent of node isn't a snarl or chain");
//    }
//
}
    


}
