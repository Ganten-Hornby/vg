#ifndef VG_SEED_CLUSTERER_HPP_INCLUDED
#define VG_SEED_CLUSTERER_HPP_INCLUDED

#include "snarls.hpp"
#include "snarl_distance_index.hpp"
#include "hash_map.hpp"
#include "small_bitset.hpp"
#include <structures/union_find.hpp>


namespace vg{

class NewSnarlSeedClusterer {



    public:

        /// Seed information used in Giraffe.
        struct Seed {
            pos_t  pos;
            size_t source; // Source minimizer.

            //For nodes
            //payload is node length, component (of the root but may be inf if the parent isnt a top-level chain), prefix sum, chain component, and is_reversed 
            tuple<size_t, size_t, size_t, size_t, bool> payload = make_tuple (std::numeric_limits<size_t>::max(),
                                                   std::numeric_limits<size_t>::max(),
                                                   std::numeric_limits<size_t>::max(),
                                                   std::numeric_limits<size_t>::max(),
                                                   false);

        };

        /// Cluster information used in Giraffe.
        struct Cluster {
            std::vector<size_t> seeds; // Seed ids.
            size_t fragment; // Fragment id.
            double score; // Sum of scores of distinct source minimizers of the seeds.
            double coverage; // Fraction of read covered by the seeds.
            SmallBitset present; // Minimizers that are present in the cluster.
        };

        NewSnarlSeedClusterer(const SnarlDistanceIndex& distance_index, const HandleGraph* graph);

        //TODO: I don't want to be too tied to the minimizer_mapper implementation with seed structs

        ///Given a vector of seeds and a distance limit, 
        //cluster the seeds such that two seeds whose minimum distance
        //between them (including both of the positions) is less than
        // the distance limit are in the same cluster

        vector<Cluster> cluster_seeds ( const vector<Seed>& seeds, size_t read_distance_limit) const;
        
        ///The same thing, but for paired end reads.
        //Given seeds from multiple reads of a fragment, cluster each read
        //by the read distance and all seeds by the fragment distance limit.
        //fragment_distance_limit must be greater than read_distance_limit
        //Returns clusters for each read and clusters of all the seeds in all reads
        //The read clusters refer to seeds by their indexes in the input vectors of seeds
        //The fragment clusters give seeds the index they would get if the vectors of
        // seeds were appended to each other in the order given
        // TODO: Fix documentation
        // Returns: For each read, a vector of clusters.

        vector<vector<Cluster>> cluster_seeds ( 
                const vector<vector<Seed>>& all_seeds, size_t read_distance_limit, size_t fragment_distance_limit=0) const;

    private:


        //Actual clustering function that takes a vector of pointers to seeds
        tuple<vector<structures::UnionFind>, structures::UnionFind> cluster_seeds_internal ( 
                const vector<const vector<Seed>*>& all_seeds,
                size_t read_distance_limit, size_t fragment_distance_limit=0) const;

        const SnarlDistanceIndex& distance_index;
        const HandleGraph* graph;

        enum ChildNodeType {CHAIN, SNARL, NODE};

        
        static inline string typeToString(ChildNodeType t) {
            switch (t) {
            case CHAIN:
                return "CHAIN";
            case SNARL:
                return "SNARL";
            case NODE:
                return "NODE";
            default:
                return "OUT_OF_BOUNDS";
            }
        }


        struct NodeClusters {
            //All clusters of a snarl tree node
            //The node containing this struct may be an actual node,
            // snarl/chain that is a node the parent snarl's netgraph,
            // or a snarl in a chain

            //The snarl tree node that the clusters are on
            SnarlDistanceIndex::CachedNetHandle containing_net_handle; 

            //Only set these for nodes
            nid_t node_id = 0;
            bool is_reversed_in_parent = false;;

            

            //set of the indices of heads of clusters (group ids in the 
            //union find)
            //TODO: Add cluster distances here
            //maps pair of <read index, seed index> to pair of <left distance, right distance>
            hash_set<pair<size_t, size_t>> read_cluster_heads;

            //The shortest distance from any seed in any cluster to the 
            //left/right end of the snarl tree node that contains these
            //clusters
            size_t fragment_best_left = std::numeric_limits<size_t>::max();
            size_t fragment_best_right = std::numeric_limits<size_t>::max();
            vector<size_t> read_best_left;
            vector<size_t> read_best_right;

            //Distance from the start of the parent to the left of this node, etc
            size_t distance_start_left = std::numeric_limits<size_t>::max();
            size_t distance_start_right = std::numeric_limits<size_t>::max();
            size_t distance_end_left = std::numeric_limits<size_t>::max();
            size_t distance_end_right = std::numeric_limits<size_t>::max();

            //Constructor
            //read_count is the number of reads in a fragment (2 for paired end)
            NodeClusters( SnarlDistanceIndex::CachedNetHandle net, size_t read_count) :
                containing_net_handle(std::move(net)),
                fragment_best_left(std::numeric_limits<size_t>::max()), fragment_best_right(std::numeric_limits<size_t>::max()),
                read_best_left(read_count, std::numeric_limits<size_t>::max()), 
                read_best_right(read_count, std::numeric_limits<size_t>::max()){}
            NodeClusters( SnarlDistanceIndex::CachedNetHandle net, size_t read_count, bool is_reversed_in_parent, nid_t node_id) :
                containing_net_handle(net),
                is_reversed_in_parent(is_reversed_in_parent),
                node_id(node_id),
                fragment_best_left(std::numeric_limits<size_t>::max()), fragment_best_right(std::numeric_limits<size_t>::max()),
                read_best_left(read_count, std::numeric_limits<size_t>::max()), 
                read_best_right(read_count, std::numeric_limits<size_t>::max()){}
        };


        struct TreeState {
            //Hold all the tree relationships, seed locations, and cluster info

            //for the current level of the snarl tree and the parent level
            //As clustering occurs at the current level, the parent level
            //is updated to know about its children

            //Vector of all the seeds for each read
            const vector<const vector<Seed>*>* all_seeds; 

            //prefix sum vector of the number of seeds per read
            //To get the index of a seed for the fragment clusters
            vector<size_t> read_index_offsets;

            //The minimum distance between nodes for them to be put in the
            //same cluster
            size_t read_distance_limit;
            size_t fragment_distance_limit;


            //////////Data structures to hold clustering information

            //Structure to hold the clustering of the seeds
            vector<structures::UnionFind> read_union_find;
            structures::UnionFind fragment_union_find;

            //For each seed, store the distances to the left and right ends
            //of the netgraph node of the cluster it belongs to
            //These values are only relevant for seeds that represent a cluster
            //in union_find_reads
            vector<vector<pair<size_t,size_t>>> read_cluster_heads_to_distances;


            //////////Data structures to hold snarl tree relationships
            //The snarls and chains get updated as we move up the snarl tree

            //Maps each node to a vector of the seeds that are contained in it
            //seeds are represented by indexes into the seeds vector
            //The array is sorted.
            vector<vector<pair<id_t, size_t>>> node_to_seeds;

            //This stores all the node clusters so we stop spending all our time allocating lots of vectors of NodeClusters
            vector<NodeClusters> all_node_clusters;

            //Map from each snarl's net_handle_t to it's child net_handle_ts 
            //clusters at the node
            //size_t is the index into all_node_clusters
            hash_map<net_handle_t, pair<size_t, vector<size_t>>> snarl_to_children;
            
            //Map each chain to the snarls (only ones that contain seeds) that
            //comprise it. 
            //Snarls and chains represented as their indexes into 
            //distance_index.chain/snarl_indexes
            //Map maps the rank of the snarl to the snarl and snarl's clusters
            //  Since maps are ordered, it will be in the order of traversal
            //  of the snarls in the chain
            //  size_t is the index into all_node_clusters
            hash_map<net_handle_t, pair<size_t, vector<size_t>>> chain_to_children;


            //TODO: Not using this anymore
            //Same structure as chain_to_children but for the level of the snarl
            //tree above the current one
            //This gets updated as the current level is processed
            //size_t is the index into all_node_clusters
            //hash_map<net_handle_t,vector<size_t>> parent_chain_to_children;

            //This holds all the child clusters of the root
            //size_t is the index into all_node_clusters
            vector<size_t> root_children;


            /////////////////////////////////////////////////////////

            //Constructor takes in a pointer to the seeds, the distance limits, and 
            //the total number of seeds in all_seeds
            TreeState (const vector<const vector<Seed>*>* all_seeds, size_t read_distance_limit, 
                       size_t fragment_distance_limit, size_t seed_count) :
                all_seeds(all_seeds),
                read_distance_limit(read_distance_limit),
                fragment_distance_limit(fragment_distance_limit),
                fragment_union_find (seed_count, false),
                read_index_offsets(1,0),
                read_cluster_heads_to_distances(all_seeds->size()){

                for (size_t i = 0 ; i < all_seeds->size() ; i++) {
                    size_t size = all_seeds->at(i)->size();
                    size_t offset = read_index_offsets.back() + size;
                    read_index_offsets.push_back(offset);
                    node_to_seeds.emplace_back();
                    node_to_seeds.back().reserve(size);
                    read_union_find.emplace_back(size, false);

                    read_cluster_heads_to_distances[i] = vector<pair<size_t,size_t>>(size, make_pair(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()));
                }

                all_node_clusters.reserve(seed_count);
                snarl_to_children.reserve(seed_count);
                chain_to_children.reserve(seed_count);
            }
        };

        //Find which nodes contain seeds and assign those nodes to the 
        //snarls that contain them
        //Update the tree state's node_to_seed
        //and snarl_to_nodes_by_level, which assigns each node that contains
        //seeds to a snarl, organized by the level of the snarl in the snarl 
        //tree. snarl_to_nodes_by_level will be used to populate snarl_to_nodes
        //in the tree state as each level is processed
        //size_t is the index into all_node_clusters
        void get_nodes( TreeState& tree_state,
                        vector<hash_map<net_handle_t, pair<size_t, vector<size_t>>>>& chain_to_children_by_level) const;

        //Add the ancestors of the node to the snarl tree. The node already has a d
        //NodeClusters in all_node_clusters, at index node_index
        //Get all relevant distances
        void add_to_snarl_tree(size_t node_index, net_handle_t& parent_handle, size_t depth, TreeState& tree_state, 
                        vector<hash_map<net_handle_t, pair<size_t, vector<size_t>>>>& chain_to_children_by_level) const;

        //Cluster all the snarls at the current level and update the tree_state
        //to add each of the snarls to the parent level
        void cluster_snarl_level(TreeState& tree_state) const;

        //Cluster all the chains at the current level
        void cluster_chain_level(TreeState& tree_state) const;

        //Cluster the seeds on the specified node
        void cluster_one_node(TreeState& tree_state, NodeClusters& node_clusters) const; 

        //Cluster the seeds in a snarl given by its net handle
        void cluster_one_snarl(TreeState& tree_state, NodeClusters& snarl_clusters) const;

        //Cluster the seeds in a chain given by chain_index_i, an index into
        //distance_index.chain_indexes
        //If the depth is 0, also incorporate the top-level seeds from tree_state.top_level_seed_clusters
        void cluster_one_chain(TreeState& tree_state, NodeClusters& chain_clusters) const;

        //Cluster in the root 
        void cluster_root(TreeState& tree_state) const;

        //Compare two children of the parent and combine their clusters, to create clusters in the parent
        //This assumes that the first node hasn't been seen before but the second one has, so all of the
        //first node's clusters get added to the parent but assume that all of the second ones are already
        //part of the parent
        //old_distances contains the distances for cluster heads in the children, 
        //since the distances in tree_state.read_cluster_heads_to_distances will get updated
        void compare_and_combine_cluster_on_child_structures(TreeState& tree_state, NodeClusters& child_clusters1,
                NodeClusters& child_clusters2, NodeClusters& parent_clusters, 
                vector<vector<pair<size_t, size_t>>>& child_distances, 
                bool is_root=false) const;

};
}

#endif
