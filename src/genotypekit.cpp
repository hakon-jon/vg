#include "genotypekit.hpp"
#include "algorithms/topological_sort.hpp"
#include "algorithms/is_directed_acyclic.hpp"
#include "cactus.hpp"

//#define debug

namespace vg {

using namespace std;

pair<const Edge*, bool> AugmentedGraph::base_edge(const Edge* edge) {
    assert(base_graph != NULL);

    bool is_trivial = false;
    const Edge* found_edge = NULL;

    // check if from node is even in the base graph
    Position from_pos;
    from_pos.set_node_id(edge->from());
    if (!translator.has_translation(from_pos)) {
        return pair<const Edge*, bool>(NULL, false);
    }

    // check if the to node is even in the base graph
    Position to_pos;
    to_pos.set_node_id(edge->to());
    if (!translator.has_translation(to_pos)) {
        return pair<const Edge*, bool>(NULL, false);        
    }

    // work in forward strand since translator doesn't seem strand-aware
    from_pos.set_is_reverse(false);
    from_pos.set_offset(edge->from_start() ? 0 :
                        graph.get_node(edge->from())->sequence().length() - 1);

    to_pos.set_is_reverse(false);
    to_pos.set_offset(edge->to_end() == false ? 0 :
                      graph.get_node(edge->to())->sequence().length() - 1);

    // Map to the base graph using our translation table
    Position base_from_pos = translator.translate(from_pos);
    Position base_to_pos = translator.translate(to_pos);

    // Make sure we don't have an unexpected reversal
    assert(base_from_pos.is_reverse() == false);
    assert(base_to_pos.is_reverse() == false);

    // Test if we're a trivial edge in the base graph (just to consecutive positions)
    if (base_from_pos.node_id() == base_to_pos.node_id() &&
        base_from_pos.is_reverse() == base_to_pos.is_reverse() &&
        abs(base_from_pos.offset() - base_to_pos.offset()) == 1) {
        is_trivial = true;
    }
    // 2) or an existing edge in the base graph (translations are all forward strand,
    //    so we need to look up sequence lengths to check if we're coming out end)
    else if (edge->from_start() && !edge->to_end() &&
             base_from_pos.offset() == 0 &&
             base_to_pos.offset() == 0) {
        found_edge = base_graph->get_edge(NodeSide(base_from_pos.node_id(), false),
                                          NodeSide(base_to_pos.node_id(), false));
    }
    else if (edge->from_start() && edge->to_end() &&
             base_from_pos.offset() == 0 &&
             base_to_pos.offset() == base_graph->get_node(base_to_pos.node_id())->sequence().length() - 1) {
        found_edge = base_graph->get_edge(NodeSide(base_from_pos.node_id(), false),
                                          NodeSide(base_to_pos.node_id(), true));
    }
    else if (!edge->from_start() && !edge->to_end() &&
             base_from_pos.offset() == base_graph->get_node(base_from_pos.node_id())->sequence().length() - 1 &&
             base_to_pos.offset() == 0) {
        found_edge = base_graph->get_edge(NodeSide(base_from_pos.node_id(), true),
                                          NodeSide(base_to_pos.node_id(), false));
    }
    else if (!edge->from_start() && edge->to_end() &&
             base_from_pos.offset() == base_graph->get_node(base_from_pos.node_id())->sequence().length() - 1 &&
             base_to_pos.offset() == base_graph->get_node(base_to_pos.node_id())->sequence().length() - 1) {
        found_edge = base_graph->get_edge(NodeSide(base_from_pos.node_id(), true),
                                          NodeSide(base_to_pos.node_id(), true));
    }

    return pair<const Edge*, bool>(found_edge, is_trivial);
}

vector<const Alignment*> AugmentedGraph::get_alignments(id_t node_id) const {
    if (alignments_by_node.count(node_id)) {
        auto& found = alignments_by_node.at(node_id);
        return vector<const Alignment*>{found.begin(), found.end()};
    } else {
        return {};
    }
}

vector<const Alignment*> AugmentedGraph::get_alignments() const {
    vector<const Alignment*> to_return;
    to_return.reserve(embedded_alignments.size());
    for (auto& alignment : embedded_alignments) {
        to_return.push_back(&alignment);
    }
    return to_return;
}

void AugmentedGraph::clear() {
    // Reset to default state
    *this = AugmentedGraph();
}

void AugmentedGraph::augment_from_alignment_edits(vector<Alignment>& alignments,
                                                  bool unique_names, bool leave_edits) {

    // This should only be called once.
    assert(embedded_alignments.empty());
    
    if (unique_names) { 
        // Make sure they have unique names.
        set<string> names_seen;
        // We warn about duplicate names, but only once.
        bool duplicate_names_warned = false;
        for(size_t i = 0; i < alignments.size(); i++) {
            if(alignments[i].name().empty()) {
                // Generate a name
                alignments[i].set_name("_unnamed_alignment_" + to_string(i));
            }
            if(names_seen.count(alignments[i].name())) {
                // This name is duplicated
                if(!duplicate_names_warned) {
                    // Warn, but only once
                    cerr << "Warning: duplicate alignment names present! Example: " << alignments[i].name() << endl;
                    duplicate_names_warned = true;
                }

                // Generate a new name
                // TODO: we assume this is unique
                alignments[i].set_name("_renamed_alignment_" + to_string(i));
                assert(!names_seen.count(alignments[i].name()));
            }
            names_seen.insert(alignments[i].name());
        }
        names_seen.clear();
    }
    
    for(auto& alignment : alignments) {
        // Trim the softclips off of every read
        // Work out were to cut
        int cut_start = softclip_start(alignment);
        int cut_end = softclip_end(alignment);
        // Cut the sequence and quality
        alignment.set_sequence(alignment.sequence().substr(cut_start, alignment.sequence().size() - cut_start - cut_end));
        if (alignment.quality().size() != 0) {
            alignment.set_quality(alignment.quality().substr(cut_start, alignment.quality().size() - cut_start - cut_end));
        }
        // Trim the path
        *alignment.mutable_path() = trim_hanging_ends(alignment.path());
    }
    
    if (!leave_edits) {
        // We want to actually modify the graph to encompass these reads.
        
        // To make the edits and copy them to/from the Alignments, we need a vector of just Paths.
        // TODO: improve this interface!
        vector<Path> paths;
        paths.reserve(alignments.size());
        for (auto& alignment : alignments) {
            paths.push_back(alignment.path());
        }
    
        // Run them through vg::edit() to modify the graph, but don't embed them
        // as paths. Update the paths in place, and save the translations.
        vector<Translation> augmentation_translations = graph.edit(paths, false, true, false);
        
        for (size_t i = 0; i < paths.size(); i++) {
            // Copy all the modified paths back.
            *alignments[i].mutable_path() = paths[i];
        }
        paths.clear();
        
        // Send out the translation
        translator.load(augmentation_translations);
    } else {
        // No need to add edits from the reads to the graph. We may have done it already with vg augment.
#ifdef debug
        cerr << "Don't add edits!" << endl;
#endif
    }
    
    // Steal the alignments for ourselves.
    std::swap(alignments, embedded_alignments);
    
    // Prepare the index from node ID to alignments that touch the node
    for (auto& alignment : embedded_alignments) {
        // For every alignment, grab its path
        const auto& path = alignment.path();
        
        // Keep track of the nodes we already saw it visit so we don't add it twice for a node.
        unordered_set<id_t> seen;
        
        for (const auto& mapping : path.mapping()) {
            // For each mapping
            if (!seen.count(mapping.position().node_id())) {
                // If it is to a new node, remember that the alignment touches the node
                seen.insert(mapping.position().node_id());
                alignments_by_node[mapping.position().node_id()].push_back(&alignment);
            }
        }
    }
    
}

void AugmentedGraph::load_translations(istream& in_file) {
    translator.translations.clear();
    function<void(Translation&)> lambda = [&](Translation& translation) {
        translator.translations.push_back(translation);
    };
    stream::for_each(in_file, lambda);
    translator.build_position_table();
}

void AugmentedGraph::write_translations(ostream& out_file) {
    stream::write_buffered(out_file, translator.translations, 0);
}

void SupportAugmentedGraph::clear() {
    // Reset to default state
    *this = SupportAugmentedGraph();
}

bool SupportAugmentedGraph::has_supports() {
    return !node_supports.empty() || !edge_supports.empty();
}

Support SupportAugmentedGraph::get_support(Node* node) {
    return node_supports.count(node) ? node_supports.at(node) : Support();
}

Support SupportAugmentedGraph::get_support(Edge* edge) {
    return edge_supports.count(edge) ? edge_supports.at(edge) : Support();
}

void SupportAugmentedGraph::load_supports(istream& in_file) {
    node_supports.clear();
    edge_supports.clear();
    function<void(LocationSupport&)> lambda = [&](LocationSupport& location_support) {
        if (location_support.oneof_location_case() == LocationSupport::kNodeId) {
            node_supports[graph.get_node(location_support.node_id())] = location_support.support();
        } else {
            const Edge& edge = location_support.edge();
            edge_supports[graph.get_edge(NodeSide(edge.from(), !edge.from_start()),
                                         NodeSide(edge.to(), edge.to_end()))] = location_support.support();
        }
    };
    stream::for_each(in_file, lambda);    
}

void SupportAugmentedGraph::write_supports(ostream& out_file) {
    vector<LocationSupport> buffer;
    for (auto& node_support : node_supports) {
        LocationSupport location_support;
        *location_support.mutable_support() = node_support.second;
        location_support.set_node_id(node_support.first->id());
        buffer.push_back(location_support);
        stream::write_buffered(out_file, buffer, 500);
    }
    for (auto& edge_support : edge_supports) {
        LocationSupport location_support;
        *location_support.mutable_support() = edge_support.second;        
        *location_support.mutable_edge() = *edge_support.first;
        buffer.push_back(location_support);
        stream::write_buffered(out_file, buffer, 500);
    }
    stream::write_buffered(out_file, buffer, 0);
}

SimpleConsistencyCalculator::~SimpleConsistencyCalculator(){

}

vector<bool> SimpleConsistencyCalculator::calculate_consistency(const Snarl& site,
        const vector<SnarlTraversal>& traversals, const Alignment& read) const {

            std::function<set<int64_t>(Alignment a, SnarlTraversal s)> shared_sites = [&](Alignment a, SnarlTraversal s){
                set<int64_t> aln_ids;
                set<int64_t> trav_ids;
                for (int i = 0; i < a.path().mapping_size(); i++){
                    Mapping m = a.path().mapping(i);
                    Position pos = m.position();
                    if (pos.node_id() != 0){
                        aln_ids.insert(pos.node_id());
                    }
                }

                for (int i = 0; i < s.visit_size(); ++i){
                    Visit v = s.visit(i);
                    if (v.node_id() != 0){
                        trav_ids.insert(v.node_id());
                    }

                }
                vector<int64_t> ret;
                std::set_intersection(aln_ids.begin(), aln_ids.end(),
                        trav_ids.begin(), trav_ids.end(),
                        std::back_inserter(ret));
                
                return set<int64_t>(ret.begin(), ret.end());
            };


            //create a consistency bool for each traversal (i.e. possible path / theoretical allele)
            vector<bool> consistencies(traversals.size());

            // For each traversal
            for (int i = 0; i < traversals.size(); ++i){
                SnarlTraversal trav = traversals[i];
                // Our snarltraversals run forward (i.e. along increading node_ids)
                // Our Alignment path may run forward OR backward.
                // We can check if an alignment is on the reverse strand and
                // flip its path around to match our snarltraversal direction.

                // Cases of consistency:
                // 1. A read maps to either end of the snarl but no internal nodes
                // 2. A read maps to both ends of the snarl but no internal nodes
                // 3. A read maps to one end of the snarl and some internal nodes.
                // 4. A read maps to both ends of the snarl and some internal nodes.
                // 5. A read maps to internal nodes, but not the snarl ends
                // A read may map to a node multiple times, or it may skip a node
                // and put an insert there.
                set<int64_t> common_ids = shared_sites(read, trav);
                bool maps_to_front = common_ids.count(trav.visit(0).node_id());
                bool maps_to_end = common_ids.count(trav.visit(trav.visit_size() - 1).node_id());
                bool maps_internally = false;

                Path read_path;
                std::function<Path(Path)> reverse_path = [&](Path p){
                    Path ret;
                    for (int i = p.mapping_size() - 1; i >= 0; i--){
                        Mapping* new_mapping = ret.add_mapping();
                        Position* pos = new_mapping->mutable_position();
                        pos->set_node_id(p.mapping(i).position().node_id());
                        int offset = p.mapping(i).position().offset();
                        pos->set_offset(offset);
                        pos->set_is_reverse(!p.mapping(i).position().is_reverse());
                        for (int j = 0; j < p.mapping(i).edit_size(); j++){
                            Edit* new_edit = new_mapping->add_edit();
                        }
                    }
                    return ret;
                };
                if (false){
                    read_path = reverse_path(read.path());
                }
                else{
                    read_path = read.path();
                }

                bool is_forward = true;
                bool is_right = true;

                if ((common_ids.size() > 1 && (maps_to_front | maps_to_end)) ||
                        common_ids.size() > 2){
                        maps_internally = true;
                }

                if (maps_to_front && maps_to_end && maps_internally){
                    // The read is anchored on both ends of the Snarl. Check
                    // the internal nodes of the Path for matches against the SnarlTraversal..

                    consistencies[i] = true;
                }
                else if (maps_to_front && maps_to_end){
                    // the read may represent a deletion,
                    // which may be in our list of traversals
                    // Either way, it's consistent with a valid traversal

                    if (true){
                        consistencies[i] = true;
                    }
                    else{
                        consistencies[i] = false;
                    }

                }
                else if (maps_to_front && maps_internally){
                    // The read maps to either the first or last node of the Snarl
                    // Check its internal nodes for matches between path and snarl


                    consistencies[i] = true;
                }
                else if (maps_to_end && maps_internally){


                    consistencies[i] = true;
                }
                else if (maps_to_front | maps_to_end){
                    // maps to the front or end, but no internal nodes.
                    // The read cannot be informative for any SnarlTraversal in this case.
                    consistencies[i] = false;
                    continue;
                }
                else{
                    // maps to neither front nor end
                    // The read could map internally, or not at all.
                    // Unless we know that the internal sequence is unique we can't guaratee that
                    // the mapping is consistent.
                    consistencies[i] = false;

                }

            }
}

SimpleTraversalSupportCalculator::~SimpleTraversalSupportCalculator(){

}

vector<Support> SimpleTraversalSupportCalculator::calculate_supports(const Snarl& site,
        const vector<SnarlTraversal>& traversals, const vector<Alignment*>& reads,
        const vector<vector<bool>>& consistencies) const{
        // Calculate the number of reads that support
        // the Traversal, and how they support it.    
        vector<Support> site_supports(traversals.size());

        for (int i = 0; i < reads.size(); i++){
            vector<bool> cons = consistencies[i];
            for (int t = 0; t < traversals.size(); t++){
                Support s;
                if (cons[t] == true && !(reads[i]->read_on_reverse_strand())){
                   s.set_forward(s.forward() + 1);
                }
                else if (cons[t] == true && reads[i]->read_on_reverse_strand()){
                    s.set_reverse(s.reverse() + 1);
                }
                else{
                    continue;
                }
            }
            
        }

        return site_supports;
        }

PathBasedTraversalFinder::PathBasedTraversalFinder(vg::VG& g, SnarlManager& sm) : graph(g), snarlmanager(sm){
}
vector<SnarlTraversal> PathBasedTraversalFinder::find_traversals(const Snarl& site){
    // Goal: enumerate traversals in the snarl supported by paths in the graph
    // that may not cover the ends of the snarl.
    // Label the Snarl's name as tthe hash of the variant and the SnarlTraversal's name
    // as the name of the alt_path (i.e. "_alt_[a-z0-9]*_[0-9]*")s
    vector<SnarlTraversal> ret;

    // If the snarl is not an ultrabubble, just return an empty set of traversals.
    if (!site.type() == ULTRABUBBLE){
        return ret;
    }
   
    // Get the Snarl's nodes
    unordered_set<int64_t> snarl_node_ids;
    pair<unordered_set<Node*>, unordered_set<Edge*> > contents = snarlmanager.shallow_contents(&site, graph, true);


    // Get the variant paths at the snarl nodes.
    set<string> var_path_names;
    regex front ("(_alt_)(.*)");
    regex alt_str ("(_alt_)");
    regex back ("(_[0-9]*)");
    map<string, list<Mapping> > gpaths = graph.paths._paths;
    set<string> gpath_names;
    for (auto x : gpaths){
        gpath_names.insert(x.first);
    }
    map<string, set<string> > basename_to_pathnames;
    map<string, bool> path_processed;


   // Collect our paths which cross our snarl's nodes.
    for (auto node : contents.first){
        //cerr << "Processing node " << id << endl;
        set<string> p_of_n = graph.paths.of_node(node->id());

        for (auto pn : p_of_n){
            if (!std::regex_match(pn, front)){
                // don't include reference paths
                continue;
            }
            string variant_hash = std::regex_replace(pn, alt_str, "");
            variant_hash = std::regex_replace(variant_hash, back, "");
            //cerr << variant_hash << endl;
            regex varbase(variant_hash);

            path_processed[pn] = false;
            basename_to_pathnames[variant_hash].insert(pn);
            var_path_names.insert(pn);
            for (auto g : gpath_names){
                if (std::regex_search(g, varbase)){
                    basename_to_pathnames[variant_hash].insert(g);
                    path_processed[g] = false;
                    var_path_names.insert(g);
                }
            }
        }
    }
    // for (auto p : var_path_names){
    //     cerr << p << endl;
    // }
    // exit(1);
    for (auto cpath : var_path_names){

        //cerr << "Working on path " << cpath << endl;
        if (!std::regex_match(cpath, front) || path_processed[cpath]){
            cerr << "Path already processed " << cpath << endl;
            continue;
        }

        if (std::regex_match(cpath, front)){
            // We found an alt path at this location
            // We need to check if it has any paths in the graph with no paths.
            string variant_hash = std::regex_replace(cpath, alt_str, "");
            variant_hash = std::regex_replace(variant_hash, back, "");
            set<string> allele_path_names = basename_to_pathnames[variant_hash];
            for (auto a : allele_path_names){
                //cerr << "Processing path " << a << endl;
                // for each allele, generate a traversal
                SnarlTraversal fresh_trav;
                fresh_trav.set_name(a);
                
                // Add the start node to the traversal
                *fresh_trav.add_visit() = site.start();
                // Fill in our traversal
                list<Mapping> ms = gpaths[a];
                for (auto m : ms){
                    int64_t n_id = m.position().node_id();
                    bool backward = m.position().is_reverse();
                    Visit* v = fresh_trav.add_visit();
                    v->set_node_id(n_id);
                    v->set_backward(backward);
                }
                // Add the end node to the traversal
                *fresh_trav.add_visit() = site.end();
                ret.push_back(fresh_trav);
                //cerr << "Finished path: " << a << endl;
                path_processed[a] = true;
            }

        }
    }
        

    // Check to make sure we got all our paths
    for (auto p : path_processed){
        if (!path_processed[p.first]){
            cerr << "VARIANT PATH MISSED: " << p.first << endl;
            exit(1617);
        }
    }

    return ret;
}

CactusSnarlFinder::CactusSnarlFinder(VG& graph) :
    graph(graph) {
    // Make sure the graph is sorted.
    algorithms::sort(&graph);
}

CactusSnarlFinder::CactusSnarlFinder(VG& graph, const string& hint_path) :
    CactusSnarlFinder(graph) {
    
    // Save the hint path
    hint_paths.insert(hint_path);
    
    // TODO: actually use it
}

SnarlManager CactusSnarlFinder::find_snarls() {
    
    if (graph.size() == 0) {
        // No snarls here!
        return SnarlManager();
    }
    // convert to cactus
    pair<stCactusGraph*, stList*> cac_pair = vg_to_cactus(graph, hint_paths);
    stCactusGraph* cactus_graph = cac_pair.first;
    stList* telomeres = cac_pair.second;

    // get the snarl decomposition as a C struct
    stSnarlDecomposition *snarls = stCactusGraph_getSnarlDecomposition(cactus_graph, telomeres);
    
    // Get a non-owning pointer to the list of chains (which are themselves lists of snarls).
    stList* cactus_chains_list = snarls->topLevelChains;
    
    // And one to the list of top-level unary snarls
    stList* cactus_unary_snarls_list = snarls->topLevelUnarySnarls;
    
    
    // We'll fill this with all the snarls
    SnarlManager snarl_manager;
    
    // Fill the manager with all of the snarls, recursively.
    recursively_emit_snarls(Visit(), Visit(), Visit(), Visit(), cactus_chains_list, cactus_unary_snarls_list, snarl_manager);
    
    // Free the decomposition
    stSnarlDecomposition_destruct(snarls);
    
    // Free the telomeres
    stList_destruct(telomeres);

    // free the cactus graph
    stCactusGraph_destruct(cactus_graph);
    
    // Return the completed SnarlManager
    return snarl_manager;
    
}

const Snarl* CactusSnarlFinder::recursively_emit_snarls(const Visit& start, const Visit& end,
    const Visit& parent_start, const Visit& parent_end,
    stList* chains_list, stList* unary_snarls_list, SnarlManager& destination) {
        
#ifdef debug    
    cerr << "Explore snarl " << start << " -> " << end << endl;
#endif
           
    // This is the snarl we are filling in to add to the SnarlManger, or an
    // empty snarl if we're a fake root snarl.
    Snarl snarl;
        
    if (start.node_id() != 0 && end.node_id() != 0) {
        // This is a real snarl
                
        // Set up the start and end
        *snarl.mutable_start() = start;
        *snarl.mutable_end() = end;
        
        if (parent_start.node_id() != 0 && parent_end.node_id() != 0) {
            // We have a parent that isn't the fake root, so fill in its ends
            *snarl.mutable_parent()->mutable_start() = parent_start;
            *snarl.mutable_parent()->mutable_end() = parent_end;
        }
    } 
    
    // This will hold the pointer to the copy of the snarl in the SnarlManager,
    // or null if the snarl is a fake root and we don't add it.
    const Snarl* managed = nullptr;
    
    // Before we can pass our snarl to the snarl manager, we need to look at all
    // its children so we can get connectivity info.
    
    // We have a vector of the snarls made for the child snarls in each ordinary
    // chain, plus trivial chains for the unary snarls.
    vector<Chain> child_chains;
    
#ifdef debug
    cerr << "Look at " << stList_length(chains_list) << " child chains" << endl;
#endif
    
    int chain_offset = 0;
    for (int64_t i = 0; i < stList_length(chains_list); i++) {
        // For each child chain
        stList* cactus_chain = (stList*)stList_get(chains_list, i);
            
        // Make a new chain
        child_chains.emplace_back();
        auto& chain = child_chains.back();
        
#ifdef debug
        cerr << "Chain " << i << " has " << stList_length(cactus_chain) << " child snarls" << endl;
#endif
        
        for (int64_t j = 0; j < stList_length(cactus_chain); j++) {
            // for each child snarl in the chain
            stSnarl* child_snarl = (stSnarl*)stList_get(cactus_chain, j);

            // scrape the vg coordinate information out of the cactus ends where we stuck
            // it during cactus construction
            CactusSide* cac_child_side1 = (CactusSide*)stCactusEdgeEnd_getObject(child_snarl->edgeEnd1);
            CactusSide* cac_child_side2 = (CactusSide*)stCactusEdgeEnd_getObject(child_snarl->edgeEnd2);
            
            // Convert from CactusSide (the interior endpoint of each node) to Visit (inward at start, outward at end)
            Visit child_start;
            child_start.set_node_id(cac_child_side1->node);
            // Start is backward if the interior is not an end
            child_start.set_backward(!cac_child_side1->is_end);
            Visit child_end;
            child_end.set_node_id(cac_child_side2->node);
            // End is backward if the interior is an end
            child_end.set_backward(cac_child_side2->is_end);
                
            // Recursively create a snarl for the child, and then add it to this chain in us.
            chain.push_back(recursively_emit_snarls(child_start, child_end, start, end,
                child_snarl->chains, child_snarl->unarySnarls, destination));
        }
    }
    
#ifdef debug
    cerr << "Look at " << stList_length(unary_snarls_list) << " child unary snarls" << endl;
#endif
    
    for (int64_t i = 0; i < stList_length(unary_snarls_list); i++) {
        // for each child unary snarl
        stSnarl* child_snarl = (stSnarl*)stList_get(unary_snarls_list, i);

        // TODO: deduplicate this code

        // scrape the vg coordinate information out of the cactus ends where we stuck
        // it during cactus construction
        CactusSide* cac_child_side1 = (CactusSide*)stCactusEdgeEnd_getObject(child_snarl->edgeEnd1);
        CactusSide* cac_child_side2 = (CactusSide*)stCactusEdgeEnd_getObject(child_snarl->edgeEnd2);
        
        // Convert from CactusSide (the interior endpoint of each node) to Visit (inward at start, outward at end)
        Visit child_start;
        child_start.set_node_id(cac_child_side1->node);
        // Start is backward if the interior is not an end
        child_start.set_backward(!cac_child_side1->is_end);
        Visit child_end;
        child_end.set_node_id(cac_child_side2->node);
        // End is backward if the interior is an end
        child_end.set_backward(cac_child_side2->is_end);
        
        // Make a trivial chain
        child_chains.emplace_back();
        auto& chain = child_chains.back();
        
        // Recursively create a snarl for the child, and then add it to the trivial chain
        chain.push_back(recursively_emit_snarls(child_start, child_end, start, end,
            child_snarl->chains, child_snarl->unarySnarls, destination));
    }

    if (snarl.start().node_id() != 0 || snarl.end().node_id() != 0) {
        // This snarl is real, we care about type and connectivity.

        // First determine connectivity
        {

            // Make a net graph for the snarl that uses internal connectivity
            NetGraph connectivity_net_graph(start, end, child_chains, &graph, true);
            
            // Evaluate connectivity
            // A snarl is minimal, so we know out start and end will be normal nodes.
            handle_t start_handle = connectivity_net_graph.get_handle(start.node_id(), start.backward());
            handle_t end_handle = connectivity_net_graph.get_handle(end.node_id(), end.backward());
            
            // Start out by assuming we aren't connected
            bool connected_start_start = false;
            bool connected_end_end = false;
            bool connected_start_end = false;
            
            // We do a couple of direcred walk searches to test connectivity.
            list<handle_t> queue{start_handle};
            unordered_set<handle_t> queued{start_handle};
            auto handle_edge = [&](const handle_t& other) {
#ifdef debug
                cerr << "\tCan reach " << connectivity_net_graph.get_id(other)
                    << " " << connectivity_net_graph.get_is_reverse(other) << endl;
#endif
                
                // Whenever we see a new node orientation, queue it.
                if (!queued.count(other)) {
                    queue.push_back(other);
                    queued.insert(other);
                }
            };
            
#ifdef debug
            cerr << "Looking for start-start turnarounds and through connections from "
                << connectivity_net_graph.get_id(start_handle) << " " <<
                connectivity_net_graph.get_is_reverse(start_handle) << endl;
#endif
            
            while (!queue.empty()) {
                handle_t here = queue.front();
                queue.pop_front();
                
                if (here == end_handle) {
                    // Start can reach the end
                    connected_start_end = true;
                }
                
                if (here == connectivity_net_graph.flip(start_handle)) {
                    // Start can reach itself the other way around
                    connected_start_start = true;
                }
                
                if (connected_start_end && connected_start_start) {
                    // No more searching needed
                    break;
                }
                
                // Look at everything reachable on a proper rightward directed walk.
                connectivity_net_graph.follow_edges(here, false, handle_edge);
            }
            
            auto end_inward = connectivity_net_graph.flip(end_handle);
            
#ifdef debug
            cerr << "Looking for end-end turnarounds from " << connectivity_net_graph.get_id(end_inward)
                << " " << connectivity_net_graph.get_is_reverse(end_inward) << endl;
#endif
            
            // Reset and search the other way from the end to see if it can find itself.
            queue = {end_inward};
            queued = {end_inward};
            while (!queue.empty()) {
                handle_t here = queue.front();
                queue.pop_front();
                
#ifdef debug
                cerr << "Got to " << connectivity_net_graph.get_id(here) << " "
                    << connectivity_net_graph.get_is_reverse(here) << endl;
#endif
                
                if (here == end_handle) {
                    // End can reach itself the other way around
                    connected_end_end = true;
                    break;
                }
                
                // Look at everything reachable on a proper rightward directed walk.
                connectivity_net_graph.follow_edges(here, false, handle_edge);
            }
            
            // Save the connectivity info. TODO: should the connectivity flags be
            // calculated based on just the net graph, or based on actual connectivity
            // within child snarls.
            snarl.set_start_self_reachable(connected_start_start);
            snarl.set_end_self_reachable(connected_end_end);
            snarl.set_start_end_reachable(connected_start_end);

#ifdef debug
            cerr << "Connectivity: " << connected_start_start << " " << connected_end_end << " " << connected_start_end << endl;
#endif
            
        
        }
        
        {
            // Determine cyclicity/acyclicity
        
            // Make a net graph that just pretends child snarls/chains are ordinary nodes
            NetGraph flat_net_graph(start, end, child_chains, &graph);
            
            // This definitely should be calculated based on the internal-connectivity-ignoring net graph.
            snarl.set_directed_acyclic_net_graph(algorithms::is_directed_acyclic(&flat_net_graph));
        }

        // Now we need to work out if the snarl can be a unary snarl or an ultrabubble or what.
        if (start.node_id() == end.node_id()) {
            // Snarl has the same start and end (or no start or end, in which case we don't care).
            snarl.set_type(UNARY);
#ifdef debug
            cerr << "Snarl is UNARY" << endl;
#endif
        } else if (!snarl.start_end_reachable()) {
            // Can't be an ultrabubble if we're not connected through.
            snarl.set_type(UNCLASSIFIED);
#ifdef debug
            cerr << "Snarl is UNCLASSIFIED because it doesn't connect through" << endl;
#endif
        } else if (snarl.start_self_reachable() || snarl.end_self_reachable()) {
            // Can't be an ultrabubble if we have these cycles
            snarl.set_type(UNCLASSIFIED);
            
#ifdef debug
            cerr << "Snarl is UNCLASSIFIED because it allows turning around, creating a directed cycle" << endl;
#endif

        } else {
            // See if we have all ultrabubble children
            bool all_ultrabubble_children = true;
            for (auto& chain : child_chains) {
                for (auto& child : chain) {
                    if (child->type() != ULTRABUBBLE) {
                        all_ultrabubble_children = false;
                        break;
                    }
                }
                if (!all_ultrabubble_children) {
                    break;
                }
            }
            
            // Note that ultrabubbles *can* loop back on their start or end.
            
            if (!all_ultrabubble_children) {
                // If we have non-ultrabubble children, we can't be an ultrabubble.
                snarl.set_type(UNCLASSIFIED);
#ifdef debug
                cerr << "Snarl is UNCLASSIFIED because it has non-ultrabubble children" << endl;
#endif
            } else if (!snarl.directed_acyclic_net_graph()) {
                // If all our children are ultrabubbles but we ourselves are cyclic, we can't be an ultrabubble
                snarl.set_type(UNCLASSIFIED);
                
#ifdef debug
                cerr << "Snarl is UNCLASSIFIED because it is not directed-acyclic" << endl;
#endif
            } else {
                // We have only ultrabubble children and are acyclic.
                // We're an ultrabubble.
                snarl.set_type(ULTRABUBBLE);
#ifdef debug
                cerr << "Snarl is an ULTRABUBBLE" << endl;
#endif
            }
        }
        
        // Now we know enough aboiut the snarl to actually put it in the SnarlManager
        managed = destination.add_snarl(snarl);
        
    }
    
    // Now add all the child chains as children of the snarl we just added (or
    // as root chains if we didn't just add a snarl)
    for (auto& chain : child_chains) {
        destination.add_chain(chain, managed);
    }

    // Return a pointer to the managed snarl.
    return managed;
}
   
ExhaustiveTraversalFinder::ExhaustiveTraversalFinder(VG& graph, SnarlManager& snarl_manager,
                                                     bool include_reversing_traversals) :
                                                     graph(graph), snarl_manager(snarl_manager),
                                                     include_reversing_traversals(include_reversing_traversals) {
    // nothing more to do
}
    
ExhaustiveTraversalFinder::~ExhaustiveTraversalFinder() {
    // no heap objects
}

void ExhaustiveTraversalFinder::stack_up_valid_walks(NodeTraversal walk_head, vector<NodeTraversal>& stack) {
    
    id_t head_id = walk_head.node->id();
    
    if (walk_head.backward) {
        // we are leaving from the start of the node
        
        // get all edges involving this node so we can filter them down to valid walks
        for (Edge* edge : graph.edges_of(walk_head.node)) {
            if (edge->from() == head_id && edge->from_start()) {
                // the edge is part of a valid walk
                Node* next_node = graph.get_node(edge->to());
                bool next_backward = edge->to_end();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
            else if (edge->to() == head_id && !edge->to_end()) {
                // the edge is part of a valid walk in the opposite orientation
                Node* next_node = graph.get_node(edge->from());
                bool next_backward = !edge->from_start();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
        }
    }
    else {
        // we are leaving from the end of the node
        
        // get all edges involving this node so we can filter them down to valid walks
        for (Edge* edge : graph.edges_of(walk_head.node)) {
            if (edge->from() == head_id && !edge->from_start()) {
                // the edge is part of a valid walk
                Node* next_node = graph.get_node(edge->to());
                bool next_backward = edge->to_end();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
            else if (edge->to() == head_id && edge->to_end()) {
                // the edge is part of a valid walk in the opposite orientation
                Node* next_node = graph.get_node(edge->from());
                bool next_backward = !edge->from_start();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
        }
    }
}

void ExhaustiveTraversalFinder::add_traversals(vector<SnarlTraversal>& traversals,
                                               NodeTraversal traversal_start,
                                               set<NodeTraversal>& stop_at,
                                               set<NodeTraversal>& yield_at) {
    // keeps track of the walk of the DFS traversal
    list<Visit> path;
    
    // these mark the start of the edges out of the node that is on the head of the path
    // they can be used to see how many nodes we need to peel off the path when we're
    // backtracking
    NodeTraversal stack_sentinel(nullptr);
    
    // initialize stack for DFS traversal of site
    vector<NodeTraversal> stack{traversal_start};
    
    while (stack.size()) {
        
        NodeTraversal node_traversal = stack.back();
        stack.pop_back();
        
        // we have traversed all of edges out of the head of the path, so we can pop it off
        if (node_traversal == stack_sentinel) {
            path.pop_back();
            continue;
        }
        
        // have we finished a traversal through the site?
        if (stop_at.count(node_traversal)) {
            if (yield_at.count(node_traversal)) {
                // yield path as a snarl traversal
                traversals.emplace_back();
                
                // record the traversal in the return value
                for (auto iter = path.begin(); iter != path.end(); iter++) {
                    *traversals.back().add_visit() = *iter;
                }
                // add the final visit
                *traversals.back().add_visit() = to_visit(node_traversal);
            }
            
            // don't proceed to add more onto the DFS stack
            continue;
        }
        
        // mark the beginning of this node's edges forward in the stack
        stack.push_back(stack_sentinel);
        
        // make a visit through the node traversal and add it to the path
        path.emplace_back();
        path.back().set_node_id(node_traversal.node->id());
        path.back().set_backward(node_traversal.backward);
        
        // does this traversal point into a child snarl?
        const Snarl* into_snarl = snarl_manager.into_which_snarl(node_traversal.node->id(),
                                                                 node_traversal.backward);
                                                                 
#ifdef debug
        cerr << "Traversal " << node_traversal.node->id() << " " << node_traversal.backward << " enters";
        if (into_snarl != nullptr) {
            cerr << " " << pb2json(*into_snarl) << endl;
        } else {
            cerr << " NULL" << endl; 
        }
#endif
                                                                 
        if (into_snarl && !(node_traversal == traversal_start)) {
            // add a visit for the child snarl
            path.emplace_back();
            *path.back().mutable_snarl()->mutable_start() = into_snarl->start();
            *path.back().mutable_snarl()->mutable_end() = into_snarl->end();
            
            // mark the beginning of this child snarls edges forward in the stack
            stack.push_back(stack_sentinel);
            
            // which side of the snarl does the traversal point into?
            if (into_snarl->start().node_id() == node_traversal.node->id()
                && into_snarl->start().backward() == node_traversal.backward) {
                // Into the start
#ifdef debug
                cerr << "Entered child through its start" << endl;
#endif
                if (into_snarl->start_end_reachable()) {
                    // skip to the other side and proceed in the orientation that the end node takes.
                    stack.push_back(to_node_traversal(into_snarl->end(), graph));
#ifdef debug
                    cerr << "Stack up " << stack.back().node->id() << " " << stack.back().backward << endl;
#endif
                }
                
                // if the same side is also reachable, add it to the stack too
                if (into_snarl->start_self_reachable()) {
                    // Make sure to flip it around so we come out of the snarl instead of going in again,
                    stack.push_back(to_rev_node_traversal(into_snarl->start(), graph).reverse());
#ifdef debug
                    cerr << "Stack up " << stack.back().node->id() << " " << stack.back().backward << endl;
#endif
                }
                
            }
            else {
                // Into the end
#ifdef debug
                cerr << "Entered child through its end" << endl;
#endif
                if (into_snarl->start_end_reachable()) {
                    // skip to the other side and proceed in the orientation
                    // *opposite* what the start node takes (i.e. out of the
                    // snarl)
                    stack.push_back(to_node_traversal(into_snarl->start(), graph).reverse());
#ifdef debug
                    cerr << "Stack up " << stack.back().node->id() << " " << stack.back().backward << endl;
#endif
                }
                
                // if the same side is also reachable, add it to the stack too
                if (into_snarl->end_self_reachable()) {
                    stack.push_back(to_rev_node_traversal(into_snarl->end(), graph));
#ifdef debug
                    cerr << "Stack up " << stack.back().node->id() << " " << stack.back().backward << endl;
#endif
                }
            }
        }
        else {
            // add all of the node traversals we can reach through valid walks to stack
            stack_up_valid_walks(node_traversal, stack);
        }
    }
}
    
vector<SnarlTraversal> ExhaustiveTraversalFinder::find_traversals(const Snarl& site) {

    vector<SnarlTraversal> to_return;
    
    NodeTraversal site_end = to_node_traversal(site.end(), graph);
    NodeTraversal site_start = to_node_traversal(site.start(), graph);
    NodeTraversal site_rev_start = NodeTraversal(site_start.node, !site_start.backward);
    
    // stop searching when the traversal is leaving the site
    set<NodeTraversal> stop_at;
    stop_at.insert(site_end);
    stop_at.insert(site_rev_start);
    
    // choose which side(s) can be the end of the traversal
    set<NodeTraversal> yield_at;
    yield_at.insert(site_end);
    if (include_reversing_traversals) {
        yield_at.insert(site_rev_start);
    }
    
    // search forward from the start and add any traversals that leave the indicated boundaries
    add_traversals(to_return, site_start, stop_at, yield_at);

    if (site.end_self_reachable() && include_reversing_traversals) {
        // if the end is reachable from itself, also look for traversals that both enter and
        // leave through the end
        yield_at.erase(site_rev_start);
        add_traversals(to_return, NodeTraversal(site_end.node, !site_end.backward),
                       stop_at, yield_at);
    }
    
    return to_return;
}
    
ReadRestrictedTraversalFinder::ReadRestrictedTraversalFinder(VG& graph, SnarlManager& snarl_manager,
                                                             const map<string, Alignment*>& reads_by_name,
                                                             int min_recurrence, int max_path_search_steps) :
                                                             graph(graph), reads_by_name(reads_by_name),
                                                             min_recurrence(min_recurrence),
                                                             max_path_search_steps(max_path_search_steps),
                                                             snarl_manager(snarl_manager) {
    // nothing else to do
}

ReadRestrictedTraversalFinder::~ReadRestrictedTraversalFinder() {
    // no heap variables
}
    
// replaces get_paths_through_site from genotyper
vector<SnarlTraversal> ReadRestrictedTraversalFinder::find_traversals(const Snarl& site) {
    // We're going to emit traversals supported by any paths in the graph.
    
    // Put all our subpaths in here to deduplicate them by sequence they spell
    // out. And to count occurrences. Note that the occurrence count will be
    // boosted to min_recurrence if a non-read path in the graph supports a
    // certain traversal string, so we don't end up dropping unsupported ref
    // alleles.
    map<string, pair<list<Visit>, int>> results;
    
#ifdef debug
#pragma omp critical (cerr)
    cerr << "Looking for paths between " << to_node_traversal(site.start(), graph) << " and " << to_node_traversal(site.end(), graph) << endl;
#endif
    
    Node* site_start_node = graph.get_node(site.start().node_id());
    Node* site_end_node = graph.get_node(site.end().node_id());
    
    if(graph.paths.has_node_mapping(site_start_node) && graph.paths.has_node_mapping(site_end_node)) {
        // If we have some paths that visit both ends (in some orientation)
        
        // Get all the mappings to the end node, by path name
        auto& endmappings_by_name = graph.paths.get_node_mapping(site_end_node);
        
        for(auto& name_and_mappings : graph.paths.get_node_mapping(site_start_node)) {
            // Go through the paths that visit the start node
            
            // Grab their names
            auto& name = name_and_mappings.first;
            
            if(!endmappings_by_name.count(name_and_mappings.first)) {
                // No path by this name has any mappings to the end node. Skip
                // it early.
                continue;
            }
            
            for(auto* mapping : name_and_mappings.second) {
                // Start at each mapping in the appropriate orientation
                
#ifdef debug
#pragma omp critical (cerr)
                cerr << "Trying mapping of read/path " << name_and_mappings.first << endl;
#endif
                
                // How many times have we gone to the next mapping looking for a
                // mapping to the end node in the right orientation?
                size_t traversal_count = 0;
                
                // Do we want to go left (true) or right (false) from this
                // mapping? If start is a forward traversal and we found a
                // forward mapping, we go right. If either is backward we go
                // left, and if both are backward we go right again.
                bool traversal_direction = mapping->position().is_reverse() != site.start().backward();
                
                // What orientation would we want to find the end node in? If
                // we're traveling backward, we expect to find it in the
                // opposite direction to the one we were given.
                bool expected_end_orientation = site.end().backward() != traversal_direction;
                
                // We're going to fill in this list with traversals.
                list<Visit> path_traversed;
                
                while(mapping != nullptr && traversal_count < max_path_search_steps) {
                    // Traverse along until we hit the end traversal or take too
                    // many steps
                    
#ifdef debug
#pragma omp critical (cerr)
                    cerr << "\tTraversing " << pb2json(*mapping) << endl;
#endif
                    
                    if(mapping->position().node_id() == site.end().node_id()
                       && mapping->position().is_reverse() == expected_end_orientation) {
                        // We have stumbled upon the end node in the orientation we wanted it in.
                        
                        path_traversed.emplace_back(site.end());
                        
                        // Construct the allele sequence from the path
                        stringstream allele_stream;
                        
                        // Don't look at the boundary nodes
                        auto iter = path_traversed.begin();
                        iter++;
                        auto end = path_traversed.end();
                        end--;
                        for (; iter != end; iter++) {
                            const Visit& visit = *iter;
                            if (visit.has_snarl()) {
                                // Add a placeholder for the nested site
                                const Snarl& child_site = visit.snarl();
                                if (visit.backward()) {
                                    allele_stream << "(" << child_site.end().node_id() << ":" << child_site.start().node_id() << ")";
                                }
                                else {
                                    allele_stream << "(" << child_site.start().node_id() << ":" << child_site.end().node_id() << ")";
                                }
                            }
                            else {
                                // Stick the sequence of the node (appropriately oriented) in the stream for the allele sequence
                                Node* node = graph.get_node(visit.node_id());
                                allele_stream << (visit.backward() ? reverse_complement(node->sequence()) : node->sequence());
                            }
                        }
                        
                        
                        if(results.count(allele_stream.str())) {
                            // It is already there! Increment the observation count.
#ifdef debug
#pragma omp critical (cerr)
                            cerr << "\tFinished; got known sequence " << allele_stream.str() << endl;
#endif
                            
                            if(reads_by_name.count(name)) {
                                // We are a read. Just increment count
                                results[allele_stream.str()].second++;
                            } else {
                                // We are a named path (like "ref")
                                if(results[allele_stream.str()].second < min_recurrence) {
                                    // Ensure that this allele doesn't get
                                    // eliminated, since ref or some other named
                                    // path supports it.
                                    results[allele_stream.str()].second = min_recurrence;
                                } else {
                                    results[allele_stream.str()].second++;
                                }
                            }
                        } else {
                            // Add it in. Give it a count of 1 if we are a read,
                            // and a count of min_recurrence (so it doesn't get
                            // filtered later) if we are a named non-read path
                            // (like "ref").
                            results[allele_stream.str()] = make_pair(path_traversed,
                                                                     reads_by_name.count(name) ? 1 : min_recurrence);
#ifdef debug
#pragma omp critical (cerr)
                            cerr << "\tFinished; got novel sequence " << allele_stream.str() << endl;
#endif
                        }
                        
//                        if(reads_by_name.count(name)) {
//                            // We want to log stats on reads that read all the
//                            // way through sites. But since we may be called
//                            // multiple times we need to send the unique read
//                            // name too.
//                            report_site_traversal(site, name);
//                        }
                        
                        // Then try the next embedded path
                        break;
                    }
                    
                    // We are not yet at the end of the of the site on this path
                    
                    // Is this traversal at the start of a nested subsite?
                    const Snarl* child_site = snarl_manager.into_which_snarl(mapping->position().node_id(),
                                                                             mapping->position().is_reverse());
                    if (child_site) {
                        
                        bool traversing_child_forward = child_site->start().node_id() == mapping->position().node_id()
                                                        && child_site->start().backward() == mapping->position().is_reverse();
                        
                        // Add the site interior to the path
                        path_traversed.emplace_back();
                        *path_traversed.back().mutable_snarl()->mutable_start() = child_site->start();
                        *path_traversed.back().mutable_snarl()->mutable_end() = child_site->end();
                        path_traversed.back().set_backward(!traversing_child_forward);
                        
                        // Find the node on the other side of the subsite and add the site interior to the allele
                        id_t site_opposite_side = traversing_child_forward ? child_site->end().node_id() : child_site->start().node_id();
                        
                        // Skip the site
                        if (traversal_direction) {
                            // Go backwards until you hit the other side of the site
                            while (mapping->position().node_id() != site_opposite_side) {
                                mapping = graph.paths.traverse_left(mapping);
                                // Break out of the loop if the path ends before crossing child site
                                if (mapping == nullptr) {
                                    break;
                                }
                                // Tick the counter so we don't go really far on long paths.
                                traversal_count++;
                            }
                        }
                        else {
                            // Go forwards until you hit the other side of the site
                            while (mapping->position().node_id() != site_opposite_side) {
                                mapping = graph.paths.traverse_right(mapping);
                                // Break out of the loop if the path ends before crossing child site
                                if (mapping == nullptr) {
                                    break;
                                }
                                // Tick the counter so we don't go really far on long paths.
                                traversal_count++;
                            }
                        }
                    }
                    else {
                        
                        // Add the visit
                        path_traversed.emplace_back();
                        path_traversed.back().set_node_id(mapping->position().node_id());
                        path_traversed.back().set_backward(mapping->position().is_reverse());
                        
                        // Otherwise just move to the right (or left) one position
                        if(traversal_direction) {
                            // We're going backwards
                            mapping = graph.paths.traverse_left(mapping);
                        } else {
                            // We're going forwards
                            mapping = graph.paths.traverse_right(mapping);
                        }
                        // Tick the counter so we don't go really far on long paths.
                        traversal_count++;
                    }
                }
            }
        }
    }
    
    // Now collect the unique results
    vector<SnarlTraversal> to_return;
    
    for(auto& result : results) {
        // Break out each result
        const string& seq = result.first;
        auto& visits = result.second.first;
        auto& count = result.second.second;
        
        if(count < min_recurrence) {
            // We don't have enough initial hits for this sequence to justify
            // trying to re-align the rest of the reads. Skip it. Note that the
            // reference path (and other named paths) will stuff in at least
            // min_recurrence to make sure we don't throw out their alleles.
            continue;
        }
        
        // Send out each list of visits
        to_return.emplace_back();
        for (Visit& visit : visits) {
            *to_return.back().add_visit() = visit;
        }
    }
    
    return to_return;
}
    
double FixedGenotypePriorCalculator::calculate_log_prior(const Genotype& genotype) {
    // Are all the alleles the same?
    bool all_same = true;
    // What is the common allele number (-1 for unset)
    int allele_value = -1;
    for(size_t i = 0; i < genotype.allele_size(); i++) {
        // For each allele in the genotype
        if(allele_value == -1) {
            // On the first one, grab it. Everyone else will have to match.
            allele_value = genotype.allele(i);
        }
        
        if(allele_value != genotype.allele(i)) {
            // There are two distinct allele values in here
            all_same = false;
            break;
        }
    }
    
    // Return the appropriate prior depending on whether the alleles are all the
    // same (homozygous) or not (heterozygous).
    return all_same ? homozygous_prior_ln : heterozygous_prior_ln;
}

TrivialTraversalFinder::TrivialTraversalFinder(VG& graph) : graph(graph) {
    // Nothing to do!
}

vector<SnarlTraversal> TrivialTraversalFinder::find_traversals(const Snarl& site) {
    assert(site.type() == ULTRABUBBLE);
    
    // We'll fill this in and send it back
    vector<SnarlTraversal> to_return;
    
    // We don't want to be duplicating partial paths, so we store for each
    // NodeTraversal we can reach the previous NodeTraversal we can reach it
    // from.
    map<NodeTraversal, NodeTraversal> previous;
    
    list<NodeTraversal> stack{to_node_traversal(site.start(), graph)};
    
    while (!stack.empty()) { 
        // While there's still stuff on the stack
        
        // Grab the first thing
        NodeTraversal here = stack.front();
        stack.pop_front();
        
        if (here.node->id() == site.end().node_id()) {
            // Trace back a path
            list<NodeTraversal> path;
            
            while (true) {
                // Until we get to the start of the site
                
                // Put this traversal on the front of the path
                path.push_front(here);
                
                if (here.node->id() == site.start().node_id()) {
                    // Stop when we've reached the start of the site
                    break;
                }
                
                // Trace back
                here = previous.at(here);
            }
            
            // Initialize a SnarlTraversal in the return value
            to_return.emplace_back();
            
            // Translate the path into the traversal
            for (NodeTraversal node_traversal : path) {
                *(to_return.back().add_visit()) = to_visit(node_traversal);
            }
            
            // Stop early after having found one path
            break;
        } else {
            // We haven't reached the end of the site
            
            for (NodeTraversal next : graph.nodes_next(here)) {
                // Look at all the places we can go from this node
                if (previous.count(next)) {
                    // We already know how to get there.
                    continue;
                }
                
                // Remember how we got there
                previous[next] = here;
                // Explore it, depth first
                stack.push_front(next);
            }
        }
    }
    
    // When we get here, either we found a path, or there isn't one.
    return to_return;
}


RepresentativeTraversalFinder::RepresentativeTraversalFinder(SupportAugmentedGraph& augmented,
    SnarlManager& snarl_manager, size_t max_depth, size_t max_width, size_t max_bubble_paths,
    function<PathIndex*(const Snarl&)> get_index) : augmented(augmented), snarl_manager(snarl_manager),
    max_depth(max_depth), max_width(max_width), max_bubble_paths(max_bubble_paths), get_index(get_index) {
    
    // Nothing to do!

}

Path RepresentativeTraversalFinder::find_backbone(const Snarl& site) {
    
    // TODO: this cheats and uses certain things that happen to be true about
    // the TrivialTraversalFinder in order to work.

    // Find a traversal, ignoring the fact that child sites ought to own their
    // nodes.
    TrivialTraversalFinder finder(augmented.graph);
    auto traversals = finder.find_traversals(site);
    assert(!traversals.empty());
    auto& traversal = traversals.front();
    
    // Convert it into a path that includes the boundary nodes
    Path to_return;
    for (size_t i = 0; i < traversal.visit_size(); i++) {
        *to_return.add_mapping() = to_mapping(traversal.visit(i), augmented.graph);
    }
    
    return to_return;
    
}

vector<SnarlTraversal> RepresentativeTraversalFinder::find_traversals(const Snarl& site) {
    
    // TODO: we can only do ultrabubbles right now. Other snarls may not have
    // traversals through from end to end.
    assert(site.type() == ULTRABUBBLE);
    
    const Snarl* managed_site = snarl_manager.manage(site);
    
    // We may need to make a new index for a backbone for this site, if it's not
    // on the primary path.
    unique_ptr<PathIndex> backbone_index;
    
    // See what the index for the appropriate primary path, if any, is. If we
    // get something non-null the site must be threaded on it.
    PathIndex* primary_path_index = get_index(site);
    
    if (primary_path_index == nullptr ||
        !primary_path_index->by_id.count(site.start().node_id()) ||
        !primary_path_index->by_id.count(site.end().node_id())) {
        // This site is not strung along the primary path, so we will need to
        // generate a backbone traversal of it to structure our search for
        // representative traversals (because they always want to come back to
        // the backbone as soon as possible).
        
        // TODO: we don't handle children correctly (we just glom them into
        // ourselves).
        Path backbone = find_backbone(site);
        
        // Index the backbone (but don't bother with the sequence)
        backbone_index = unique_ptr<PathIndex>(new PathIndex(backbone));
    }
    
    // Determnine what path will be the path we use to scaffold the traversals:
    // the primary path index by default, or the backbone index if we needed one.
    PathIndex& index = (backbone_index.get() != nullptr ? *backbone_index : *primary_path_index);
    
    // Get the site's nodes and edges, including our outer boundary nodes, not used inside children.
    // TODO: can we not include the child boundaries? Would that make things easier?
    pair<unordered_set<Node*>, unordered_set<Edge*>> contents = snarl_manager.shallow_contents(&site, augmented.graph, true);
    
    // Copy its node set
    unordered_set<Node*> nodes_left(contents.first);

    // Trace the ref path through the site.
    vector<Visit> ref_path_for_site;
    
    // First figure where the site starts and ends in the selected path
    size_t site_start = index.by_id.at(site.start().node_id()).first;
    size_t site_end = index.by_id.at(site.end().node_id()).first;
    
#ifdef debug
    cerr << "Site starts with " << to_node_traversal(site.start(), augmented.graph)
        << " at " << site_start
        << " and ends with " << to_node_traversal(site.end(), augmented.graph)
        << " at " << site_end << endl;
        
    for (auto* node : nodes_left) {
        cerr << "\tContains node " << node->id() << endl;
    }
#endif

    // The primary path may go through the site backward. So get the primary min and max coords
    size_t primary_min = min(site_start, site_end);
    size_t primary_max = max(site_start, site_end);

    // Then walk nodes from min coordinate to max coordinate. This holds the
    // start coordinate of the current node.
    int64_t ref_node_start = primary_min;
    while(ref_node_start <= primary_max) {
    
        // Find the reference node starting here or later.
        auto found = index.by_start.lower_bound(ref_node_start);
        if(found == index.by_start.end()) {
            throw runtime_error("No backbone node found when tracing through site!");
        }
#ifdef debug
        cerr << "Ref node: " << found->second << " at " << ref_node_start << "/" << primary_max << endl;
#endif
        if((*found).first > primary_max) {
            // The next reference node we can find is out of the space
            // being replaced. We're done.
            if (verbose) {
                cerr << "Stopping for out-of-bounds node" << endl;
            }
            break;
        }
        
        // Get the corresponding Visit
        Visit found_visit = found->second.to_visit();
        
        // What node did we hit?
        Node* visited_node = augmented.graph.get_node(found_visit.node_id());
        
        const Snarl* child = snarl_manager.into_which_snarl(found_visit);
        if (child != nullptr && child != managed_site
            && snarl_manager.into_which_snarl(reverse(found_visit)) != managed_site) {
            // If the node in this orientation enters a child
        
            // Visit the child
            Visit child_visit;
            *child_visit.mutable_snarl()->mutable_start() = child->start();
            *child_visit.mutable_snarl()->mutable_end() = child->end();
            if (found_visit == child->start()) {
                // We enter the child on its left
                child_visit.set_backward(false);
            } else {
                assert(found_visit == reverse(child->end()));
                // We enter the child on its right
                child_visit.set_backward(true);
            }
            ref_path_for_site.push_back(child_visit);
        
            // And skip to its other end.
            // TODO: the path is not allowed to end inside the snarl.
            Node* here = visited_node;
            do {
#ifdef debug
                cerr << "at node " << pb2json(*here) << endl;
#endif
                // Advance
                ref_node_start = found->first + here->sequence().size();
                // And look at what we get
                found = index.by_start.lower_bound(ref_node_start);
                assert(found != index.by_start.end());
                // And grab out the node
                found_visit = found->second.to_visit();
                here = augmented.graph.get_node(found_visit.node_id());
                // Until we find something in this parent again that isn't the
                // closing visit of a child snarl. We'll look at what we find
                // next.
            } while (!contents.first.count(here));
            
            if (snarl_manager.into_which_snarl(reverse(found_visit)) != nullptr) {
                // We hit the end node of the child snarl.
                
                if (snarl_manager.into_which_snarl(found_visit) == nullptr) {
                    // We don't have another child snarl immediately. Look at the node after this one.
                    ref_node_start = found->first + here->sequence().size();
                    found = index.by_start.lower_bound(ref_node_start);
                    assert(found != index.by_start.end());
                    found_visit = found->second.to_visit();
                    here = augmented.graph.get_node(found_visit.node_id());
                } else {
                    // It's also the start node of another child snarl, so loop
                    // on it again. Do nothing here.
#ifdef debug
                    cerr << "Back-to-back child snarls!" << endl;
#endif
                }
            }
            
            // Make sure we actually found something meeting those criteria.
            // TODO: the path is not allowed to end inside the snarl.
            assert(contents.first.count(here));
        } else {
            // Otherwise, visit this node
            
            if (nodes_left.count(visited_node)) {
                // If the node is one we still expect to see, drop it from the
                // set of nodes in the site we haven't visited.
                nodes_left.erase(visited_node);
            }
            
            // Add the traversal to the ref path through the site
            ref_path_for_site.push_back(found_visit);
            
            
            // Next iteration look where this node ends.
            ref_node_start = found->first + visited_node->sequence().size();
        }
        
#ifdef debug
        cerr << "Added visit: " << pb2json(ref_path_for_site.back()) << endl; 
#endif   
    }
    
    // We leave the ref path in backbone-relative forward orientation, because
    // all our bubbles we find will also be in backbone-relative forward
    // orientation.
    
    for(auto node : nodes_left) {
        // Make sure none of the nodes in the site that we didn't visit
        // while tracing along the ref path are on the ref path.
        
        if (snarl_manager.into_which_snarl(node->id(), true) || snarl_manager.into_which_snarl(node->id(), false)) {
            // Skip child boundary nodes.
            continue;
        }
        
        if(index.by_id.count(node->id())) {
            cerr << "Node " << node->id() << " is on backbone path at "
                << index.by_id.at(node->id()).first << " but not traced in site "
                << to_node_traversal(site.start(), augmented.graph) << " to " 
                << to_node_traversal(site.end(), augmented.graph) << " that contains it." << endl;
            throw runtime_error("Extra ref node found");
        }
    }
    
    // We need to know all the full-length traversals we're going to consider.
    // XREF states will have to be calculated later, over the whole traversal.
    set<vector<Visit>> site_traversal_set;
    
    // We have this function to extend a partial traversal into a full
    // traversal and add it as a path. The path must already be rooted on
    // the reference in the correct order and orientation.
    auto extend_into_allele = [&](vector<Visit> path) {
        // Splice the ref path through the site and the bubble's path
        // through the site together.
        vector<Visit> extended_path;
#ifdef debug
        cerr << "Input path: " << endl;
        for(auto& visit : path) {
            if(visit.node_id() != 0 && index.by_id.count(visit.node_id())) {
                cerr << "\tPath member " << visit << " lives on backbone at "
                << index.by_id.at(visit.node_id()).first << endl;
            } else {
                cerr << "\tPath member " << visit << " does not live on backbone" << endl;
            }
        }
#endif
        
        for(auto& visit : path) {
            if (visit.node_id() != 0) {
                // Make sure the site actually has the nodes we're visiting.
                assert(contents.first.count(augmented.graph.get_node(visit.node_id())));
            }
            // Child snarls will have ownership of their end nodes, so they won't be part of our contents.
        }
        
        size_t ref_path_index = 0;
        size_t ref_path_end = ref_path_for_site.size() - 1;
        size_t bubble_path_index = 0;
        size_t bubble_path_end = path.size() - 1;
        
        // Function that returns the node visit on the on one end of a visit, even
        // if the visit is of a child snarl
        auto frontier_visit = [&](const Visit& visit, bool left_side) {
            if (visit.node_id() != 0) {
                return visit;
            }
            else if (visit.backward() && !left_side) {
                return reverse(visit.snarl().start());
            }
            else if (!visit.backward() && !left_side) {
                return visit.snarl().end();
            }
            else if (visit.backward()) {
                return reverse(visit.snarl().end());
            }
            else {
                return visit.snarl().start();
            }
        };
        
        while(frontier_visit(ref_path_for_site[ref_path_index], false) != frontier_visit(path[bubble_path_index], true) &&
              !(path[bubble_path_index].node_id() == 0 &&
                frontier_visit(ref_path_for_site[ref_path_index], false) == frontier_visit(path[bubble_path_index], false))) {
            // Collect NodeTraversals from the ref path until we hit the one
            // at which the bubble path starts.
#ifdef debug
            cerr << "Before path: " << pb2json(ref_path_for_site[ref_path_index]) << endl;
#endif
            extended_path.push_back(ref_path_for_site[ref_path_index++]);
        }
        
        if (ref_path_for_site[ref_path_index].node_id() == 0) {
            // The last Visit we traversed from the ref was a Snarl, so it already
            // includes the first node of the path as one of its boundaries. We need
            // to add the ref visit and exclude the bubble visit unless it is also of
            // a child Snarl and that Snarl is different from the ref path
            
#ifdef debug
            cerr << "Adding final ref child visit " << pb2json(ref_path_for_site[ref_path_index]) << endl;
#endif
            extended_path.push_back(ref_path_for_site[ref_path_index]);
            
            if (path.front().node_id() != 0 || (path.front().snarl().start() == extended_path.back().snarl().start()
                                                && path.front().snarl().end() == extended_path.back().snarl().end())) {
#ifdef debug
                cerr << "Skipping bubble visit " << pb2json(path[bubble_path_index]) << endl;
#endif
                bubble_path_index++;
            }
        }
        
        while(bubble_path_index < path.size()) {
            // Then take the whole bubble path
#ifdef debug
            cerr << "In path: " << pb2json(path[bubble_path_index]) << endl;
#endif
            extended_path.push_back(path[bubble_path_index++]);
        }
        while(ref_path_index < ref_path_for_site.size()) {
            // Then skip ahead to the matching point in the ref path, which may
            // be either a full visit match, or a match to the exit node of a
            // visited child snarl.
            
            // Check each reference visit
            auto& ref_visit = ref_path_for_site.at(ref_path_index);
            
#ifdef debug
            cerr << "At ref: " << pb2json(ref_visit) << " with frontier visit " << pb2json(frontier_visit(ref_visit, true)) << " looking for " << pb2json(frontier_visit(path.back(), false)) << endl;
#endif
            
            if (frontier_visit(ref_visit, true) == frontier_visit(path.back(), false) ||
                (path.back().node_id() == 0 && frontier_visit(ref_visit, false) == frontier_visit(path.back(), false))) {
                // We found the exit node. Put the after-the-bubble visits
                // continuing from here.
                break;
            }
            
            // Otherwise this ref visit isn't the right one to match up with our
            // bubble's traversal.
#ifdef debug
            cerr << "Skip ref: " << pb2json(ref_path_for_site[ref_path_index]) << endl;
            cerr << "\tWant: " << pb2json(path.back()) << endl;
#endif
            ref_path_index++;
        }
        
        if(ref_path_index == ref_path_for_site.size()) {
            // We ran out of ref path before finding the place to leave the alt.
            // This must be a backtracking loop or something; start over from the beginning.
            cerr << "ran out of ref" << endl;
            ref_path_index = 0;
            
            while(ref_path_index < ref_path_for_site.size() &&
                  frontier_visit(ref_path_for_site.at(ref_path_index), true) != frontier_visit(path.back(), false)) {
                // Then skip ahead to the matching point in the ref path
                ref_path_index++;
            }
            
            if(ref_path_index == ref_path_for_site.size()) {
                // Still couldn't find it!
                stringstream err;
                err << "Couldn't find " << path.back() << " in backbone path of site "
                << site.start()
                << " to " << site.end() << endl;
                throw runtime_error(err.str());
            }
        }
        
        if (ref_path_for_site[ref_path_index].node_id() == 0) {
            // The next Visit on the ref path is to a Snarl, so the final Visit we added
            // from the bubble will be redundant with its boundary nodes unless that Visit
            // was also to a Snarl
            if (extended_path.back().node_id() != 0 ||
                (ref_path_for_site[ref_path_index].snarl().start() == extended_path.back().snarl().start()
                 && ref_path_for_site[ref_path_index].snarl().end() == extended_path.back().snarl().end())) {
#ifdef debug
                cerr << "Removing bubble visit " << pb2json(extended_path.back()) << endl;
#endif
                extended_path.pop_back();
            }
#ifdef debug
            cerr << "Adding adjacent ref child visit" << pb2json(ref_path_for_site[ref_path_index]) << endl;
#endif
            extended_path.push_back(ref_path_for_site[ref_path_index]);
        }
        // Skip the matching NodeTraversal
        ref_path_index++;
        while(ref_path_index < ref_path_for_site.size()) {
            // Then take the entier rest of the ref path
#ifdef debug
            cerr << "After path: " << pb2json(ref_path_for_site[ref_path_index]) << endl;
#endif
            extended_path.push_back(ref_path_for_site[ref_path_index++]);
        }
        
#ifdef debug
        cerr << "Output path: " << endl;
        for (auto& v : extended_path) {
            cerr << "\t" << pb2json(v) << endl;
        }
#endif
        
        // Now add it to the set
        site_traversal_set.insert(extended_path);
        
    };

#ifdef debug
    cerr << "Explore " << contents.first.size() << " nodes" << endl;
#endif

    for (Node* node : contents.first) {
        // Find the bubble for each node
        
        if (snarl_manager.into_which_snarl(node->id(), true) || snarl_manager.into_which_snarl(node->id(), false)) {
            // Don't start from nodes that are child boundaries
            continue;
        }
        
        if (augmented.has_supports() && total(augmented.get_support(node)) == 0) {
            // Don't bother with unsupported nodes
            continue;
        }
        
        if (index.by_id.count(node->id())) {
            // Don't try to pathfind to the backbone for backbone nodes.
            continue;
        }
        
#ifdef debug
        cerr << "Base path on " << node->id() << endl;
#endif
        
        // Find bubbles that backend into the backbone path
        pair<Support, vector<Visit>> sup_path = find_bubble(node, nullptr, nullptr, index, site);

        vector<Visit>& path = sup_path.second;
        
        if(path.empty()) {
            // We couldn't find a path back to the primary path. Discard
            // this material.
            if (verbose) {
                cerr << "Warning: No path found for node " << node->id() << endl;
            }
            // TODO: record the node's bases as lost.
            
            // TODO: what if it's already in another bubble/the node is deleted?
            continue;
        }
        
        // Extend it out into an allele
        extend_into_allele(path);
        
    }
    

#ifdef debug
    cerr << "Explore " << contents.second.size() << " edges" << endl;
#endif

    for(Edge* edge : contents.second) {
        // Go through all the edges
        
        if(augmented.has_supports() && total(augmented.get_support(edge)) == 0) {
            // Don't bother with unsupported edges
#ifdef debug
            cerr << "Skip unsupported edge " << edge->from() << " -> " << edge->to() << endl;
#endif
            continue;
        }
        
        if(!index.by_id.count(edge->from()) || !index.by_id.count(edge->to())) {
            // Edge doesn't touch backbone at both ends. Don't use it
            // because for some reason it makes performance worse
            // overall.
#ifdef debug
            cerr << "Skip off-backbone edge " << edge->from() << " -> " << edge->to() << endl;
#endif
            continue;
        }
        
#ifdef debug
        cerr << "Base path on " << edge->from() << " -> " << edge->to() << endl;
#endif
        
        // Find a path based around this edge
        pair<Support, vector<Visit>> sup_path = find_bubble(nullptr, edge, nullptr, index, site);
        vector<Visit>& path = sup_path.second;
        
#ifdef debug
        cerr << "Edge " << edge->from() << " to " << edge->to() << " yields:" << endl;
        for(auto& visit : path) {
            cerr << "\t" << visit << endl;
        }
#endif
        
        if(path.empty()) {
            // We couldn't find a path back to the primary path. Discard
            // this material.
            if (verbose) {
                cerr << "Warning: No path found for edge " << edge->from() << "," << edge->to() << endl;
            }
            // TODO: bases lost
            continue;
        }
        
        // Extend it out into an allele
        extend_into_allele(path);
    }
    
    auto children = snarl_manager.children_of(&site);

#ifdef debug
    cerr << "Explore " << children.size() << " children" << endl;
#endif

    for (const Snarl* child : children) {
        // Go through all the child snarls
        
#ifdef debug
        cerr << "Base path on " << *child << endl;
#endif
        
        // Find a path based around this child snarl
        pair<Support, vector<Visit>> sup_path = find_bubble(nullptr, nullptr, child, index, site);
        vector<Visit>& path = sup_path.second;
        
        if(path.empty()) {
            // We couldn't find a path back to the primary path. Discard
            // this material.
            if (verbose) {
                cerr << "Warning: No path found for child snarl " << *child << endl;
            }
            // TODO: bases lost
            continue;
        }
        
        // Extend it out into an allele
        extend_into_allele(path);
    }
    
    
    // Now convert to SnarlTraversals
    vector<SnarlTraversal> unique_traversals;
    
    // Have a function to convert a vector of NodeTraversals including the snarl
    // ends into a SnarlTraversal
    auto emit_traversal = [&](vector<Visit> visits) {
        // Make it into this SnarlTraversal
        SnarlTraversal trav;
        
#ifdef debug
        cerr << "Unique traversal's visits:" << endl;
        for(auto& visit : visits) {
            cerr << "\t" << visit << endl;
        }
#endif
        
        // Convert the path to Visits.
        // TODO: think about nested sites?
        
        if (site_start > site_end) {
            // The primary path runs backward, so we need to emit all our lists
            // of NodeTraversals backward so they come out in the snarl's
            // orientation and not the primary path's.
            
            for(size_t i = 0; i < visits.size(); i++) {
                // Record a Visit for each Visit but the first and last,
                // but backward and in reverse order.
                *trav.add_visit() = reverse(visits[visits.size() - i - 1]);
            }
            
        } else {
            // The primary path and the snarl use the same orientation
            
            for(size_t i = 0; i < visits.size(); i++) {
                // Make a Visit for each NodeTraversal but the first and last
                *trav.add_visit() = visits[i];
            }
            
        }
        
#ifdef debug
        cerr << "Unique traversal: " << pb2json(trav) << endl;
#endif

        // Save the SnarlTraversal
        unique_traversals.push_back(trav);
    };
    
    
    // Do the ref path first
    emit_traversal(ref_path_for_site);
    for(auto& visits : site_traversal_set) {
        // Look at each vector of Visits
        if (visits != ref_path_for_site) {
            // And do everything other than the ref path
            emit_traversal(visits);            
        }
        
    }
    
    return unique_traversals;
}

pair<Support, vector<Visit>> RepresentativeTraversalFinder::find_bubble(Node* node, Edge* edge,
    const Snarl* snarl, PathIndex& index, const Snarl& site) {

    // What are we going to find our left and right path halves based on?
    Visit left_visit;
    Visit right_visit;
    
    const Snarl* managed_site = snarl_manager.manage(site);
    
    if (edge != nullptr) {
        // Be edge-based
        
        // Find the nodes at the ends of the edges. Look at them traversed in the
        // edge's local orientation.
        left_visit = to_visit(edge->from(), edge->from_start());
        right_visit = to_visit(edge->to(), edge->to_end());
        
        const Snarl* right_child = snarl_manager.into_which_snarl(right_visit);
        const Snarl* left_child = snarl_manager.into_which_snarl(left_visit);
        
        if (right_child != nullptr && right_child != managed_site
            && snarl_manager.into_which_snarl(reverse(right_visit)) != managed_site) {
            // We're reading into a child snarl on the right.
#ifdef debug
            cerr << "Child to right of edge " << pb2json(*right_child) << endl;
#endif
            
            // Make a visit.
            Visit right_child_visit = to_visit(*right_child);
            
            if (to_left_side(right_visit) != to_left_side(right_child_visit)) {
                // We really go through it the other way around
                right_child_visit = reverse(right_child_visit);
            }
            assert(to_left_side(right_visit) == to_left_side(right_child_visit));
            // Use the snarl visit instead of the visit to the entering node.
            right_visit = right_child_visit;
        }
        
        if (left_child != nullptr && left_child != managed_site
            && snarl_manager.into_which_snarl(reverse(left_visit)) != managed_site) {
            // We're reading out of a child snarl on the left.
#ifdef debug
            cerr << "Child to left of edge " << pb2json(*left_child) << endl;
#endif
            
            // Make a visit.
            Visit left_child_visit = to_visit(*left_child);
            
            if (to_right_side(left_visit) != to_right_side(left_child_visit)) {
                // We really go through it the other way around
                left_child_visit = reverse(left_child_visit);
            }
            assert(to_right_side(left_visit) == to_right_side(left_child_visit));
            // Use the snarl visit instead of the visit to the exiting node.
            left_visit = left_child_visit;
        }
#ifdef debug
        cerr << "Edge becomes " << left_visit << " -> " << right_visit << endl;
#endif
        
    } else if (node != nullptr) {
        // Be node-based. TODO: we trust the caller not to feed us nodes that
        // are part of/boundaries of child snarls.
        left_visit = right_visit = to_visit(node->id(), false);
    } else {
        // Be snarl-based
        assert(snarl != nullptr);
        left_visit = right_visit = to_visit(*snarl);
    }
    
#ifdef debug
    cerr << "Starting from: " << left_visit << ", " << right_visit << endl;
#endif

    // Find paths on both sides, with nodes or snarls on the primary path at the
    // outsides and this visit in the middle. Returns path lengths and paths in
    // pairs in a set.
    auto leftPaths = bfs_left(left_visit, index, false, managed_site);
    auto rightPaths = bfs_right(right_visit, index, false, managed_site);
    
    // Find a combination of two paths which gets us to the reference in a
    // consistent orientation (meaning that when you look at the ending nodes'
    // Mappings in the reference path, the ones with minimal ranks have the same
    // orientations) and which doesn't use the same nodes on both sides.
    // Track support of up to max_bubble_paths combinations, and return the
    // highest
    pair<Support, vector<Visit> > bestBubblePath;
    int bubbleCount = 0;
    
    // We need to look in different combinations of lists.
    auto testCombinations = [&](const list<list<Visit>>& leftList,
        const list<list<Visit>>& rightList) {
        
#ifdef debug        
        cerr << "Combine " << leftList.size() << " left sides and "
            << rightList.size() << " right sides" << endl;
#endif
        
        // We know the left list starts and the right list ends with an actual
        // node visit, if only to the snarl's start or end.

        for(auto leftPath : leftList) {
            // Figure out the relative orientation for the leftmost node.
#ifdef debug        
            cerr << "Left path: " << endl;
            for(auto visit : leftPath ) {
                cerr << "\t" << visit << endl;
            }
#endif    
            
            // Find what node side actually represents the end
            auto leftSide = to_left_side(leftPath.front());
            
            // Split out the node's orientation
            bool leftOrientation = leftSide.is_end;
            
            // Get where it falls in the reference as a position, orientation pair.
            auto leftRefPos = index.by_id.at(leftSide.node);
            
            // We have a backward orientation relative to the reference path if we
            // were traversing the anchoring node backwards, xor if it is backwards
            // in the reference path.
            bool leftRelativeOrientation = leftOrientation != leftRefPos.second;
            
            // Make a set of all the nodes in the left path
            set<int64_t> leftPathNodes;
            // And one of all the snarls (with bounding visits set)
            set<Snarl> leftPathSnarls;
            for(auto visit : leftPath) {
                if (visit.node_id() != 0) {
                    // It's a node visit
                    leftPathNodes.insert(visit.node_id());
                } else {
                    // It's a snarl visit
                    leftPathSnarls.insert(visit.snarl());
                }
            }

            // Get the minimum support in the left path
            Support minLeftSupport = min_support_in_path(leftPath);
            
            for(auto rightPath : rightList) {
                // Figure out the relative orientation for the rightmost node.
#ifdef debug            
                cerr << "Right path: " << endl;
                for(auto visit : rightPath ) {
                    cerr << "\t" << visit << endl;
                }
#endif            
                
                // Find what node side actually represents the end
                // Remember it's at the end of this path.
                auto rightSide = to_right_side(rightPath.back());
                
                // Split out the node's orientation
                bool rightOrientation = !rightSide.is_end;
                
                // Get where it falls in the reference as a position, orientation pair.
                auto rightRefPos = index.by_id.at(rightSide.node);
                
                // We have a backward orientation relative to the reference path if we
                // were traversing the anchoring node backwards, xor if it is backwards
                // in the reference path.
                bool rightRelativeOrientation = rightOrientation != rightRefPos.second;

                // Get the minimum support in the right path
                Support minRightSupport = min_support_in_path(rightPath);
                
                if(leftRelativeOrientation == rightRelativeOrientation &&
                    ((!leftRelativeOrientation && leftRefPos.first < rightRefPos.first) ||
                    (leftRelativeOrientation && leftRefPos.first > rightRefPos.first))) {
                    // We found a pair of paths that get us to and from the
                    // reference without turning around, and that don't go back to
                    // the reference before they leave.

                    // Get the minimum support of combined left and right paths
                    Support minFullSupport = support_min(minLeftSupport, minRightSupport);
                    
                    // Start with the left path
                    vector<Visit> fullPath{leftPath.begin(), leftPath.end()};
                    
                    // We need to detect overlap with the left path
                    bool overlap = false;
                    
                    // If we're starting from an edge, we keep the first visit
                    // on the right path. If we're starting from a node or
                    // snarl, we need to discard it because it's just another
                    // copy of our visit we're starting with.
                    for(auto it = (edge != nullptr ? rightPath.begin() : ++(rightPath.begin())); it != rightPath.end(); ++it) {
                        // For all but the first node on the right path, add that in
                        fullPath.push_back(*it);
                        
                        if (it->node_id() != 0) {
                            // This right-side visit hits a node
                            if(leftPathNodes.count(it->node_id())) {
                                // We already visited this node on the left side. Try
                                // the next right path instead.
                                overlap = true;
                            }
                        } else {
                            // This right-side visit hits a snarl
                            if(leftPathSnarls.count(it->snarl())) {
                                // We already visited this snarl on the left side. Try
                                // the next right path instead.
                                overlap = true;
                            }
                        }
                    }
                    
                    if(overlap) {
                        // Can't combine this right with this left, as they
                        // share nodes or child snarls and we can't handle the
                        // copy number implications. Try the next right. TODO:
                        // handle the copy number implications.
                        // TODO: This shouldn't happen in ultrabubbles.
                        continue;
                    }
                    
                    if(leftRelativeOrientation) {
                        // Turns out our anchored path is backwards.
                        
                        // Reorder everything the other way
                        reverse(fullPath.begin(), fullPath.end());
                        
                        for(auto& visit : fullPath) {
                            // Flip each Visit
                            visit = reverse(visit);
                        }
                    }

                    
#ifdef debug        
                    cerr << "Merged path:" << endl;
                    for(auto visit : fullPath) {
                        cerr << "\t" << visit << endl;
                    }                    
#endif
                    // Update our best path by seeing if we've found one with
                    // higher min support. Make sure to replace the empty path
                    // even if we only find a traversal with 0 support (because
                    // maybe we have no support data at all).
                    if (total(minFullSupport) > total(bestBubblePath.first) ||
                        (total(minFullSupport) == total(bestBubblePath.first) && bestBubblePath.second.empty())) {
                        bestBubblePath.first = minFullSupport;
                        bestBubblePath.second = fullPath;
                    }

                    // keep things from getting out of hand
                    if (++bubbleCount >= max_bubble_paths) {
                        return bestBubblePath;
                    }
                }
            }
        }
        
        // Return the best path along with its min support
        // (could be empty)
        return bestBubblePath;
        
    };
    
    // Convert sets to lists, which requires a copy again...
    // TODO: Can we just completely remove the length calculation?
    list<list<Visit>> leftConverted;
    for(auto lengthAndPath : leftPaths) {
        leftConverted.emplace_back(move(lengthAndPath.second));
    }
    list<list<Visit>> rightConverted;
    for(auto lengthAndPath : rightPaths) {
        rightConverted.emplace_back(move(lengthAndPath.second));
    }
    
    // Look for a valid combination, or return an empty path if one iesn't
    // found.
    return testCombinations(leftConverted, rightConverted);
    
}

Support RepresentativeTraversalFinder::min_support_in_path(const list<Visit>& path) {
    
    if (path.empty()) {
        // No support if we visit nothing!
        return Support();
    }
    
    // Get an iterator to the current visit on the path
    auto cur = path.begin();
    // And to the next visit on the path
    auto next = path.begin();
    ++next;
    
    // Have we found anything with a support yet?
    bool supportFound = false;
    // If we have, this holds the min support we have found.
    Support minSupport;
    
    if (cur->node_id() != 0) {
        // We're at a node visit, so we have a support to start with
        minSupport = augmented.get_support(augmented.graph.get_node(cur->node_id()));
        supportFound = true;
    }
    
    for (; next != path.end(); ++cur, ++next) {
        // For each visit and its next visit
    
        if (next->node_id() != 0) {
            // The next visit is to a node, so get its support
            Support nextSupport = augmented.get_support(augmented.graph.get_node(next->node_id()));
            
            if (supportFound) {
                // Min it against existing support
                minSupport = support_min(minSupport, nextSupport);
            } else {
                // Take as the found support
                minSupport = nextSupport;
                supportFound = true;
            }
        }
        
        // TODO: Support for child snarls!
    
        // check the edge support
        Edge* edge = augmented.graph.get_edge(to_right_side(*cur), to_left_side(*next));
        
        if (edge != nullptr) {
            // The edge exists (because we aren't back-to-back child snarls)
            Support edgeSupport = augmented.get_support(edge);
            
            if (supportFound) {
                // Min it against existing support
                minSupport = support_min(minSupport, edgeSupport);
            } else {
                // Take as the found support
                minSupport = edgeSupport;
                supportFound = true;
            }
        }
    }

    // This may be 0 if we hit no nodes or edges, but I guess that's OK...
    return minSupport;
}

set<pair<size_t, list<Visit>>> RepresentativeTraversalFinder::bfs_left(Visit visit,
    PathIndex& index, bool stopIfVisited, const Snarl* in_snarl) {

    // Holds partial paths we want to return, with their lengths in bp.
    set<pair<size_t, list<Visit>>> toReturn;
    
    // Do a BFS
    
    // This holds the paths to get to NodeTraversals to visit (all of which will
    // end with the node we're starting with).
    list<list<Visit>> toExtend;
    
    // This keeps a set of all the oriented nodes we already got to and don't
    // need to queue again.
    set<Visit> alreadyQueued;
    
    // Start at this node at depth 0
    toExtend.emplace_back(list<Visit> {visit});
    // Mark this traversal as already queued
    alreadyQueued.insert(visit);
    
    // How many ticks have we spent searching?
    size_t searchTicks = 0;
    
#ifdef debug
    cerr << "Start BFS" << endl;
#endif

    // Track how many options we have because size may be O(n).
    size_t stillToExtend = toExtend.size();
    
    while (!toExtend.empty()) {
        // Keep going until we've visited every node up to our max search depth.
        
        searchTicks++;
        

#ifdef debug
        // Report on how much searching we are doing.
        cerr << "Search tick " << searchTicks << ", " << stillToExtend << " options." << endl;
#endif

        
        // Dequeue a path to extend.
        // Make sure to move out of the list to avoid a useless copy.
        list<Visit> path(move(toExtend.front()));
        toExtend.pop_front();
        stillToExtend--;
        
        // We can't just throw out longer paths, because shorter paths may need
        // to visit a node twice (in opposite orientations) and thus might get
        // rejected later. Or they might overlap with paths on the other side.
        
        // Look up and see if the front node on the path is on our reference
        // path
        if (path.front().node_id() != 0 && index.by_id.count(path.front().node_id())) {
            // This visit is to a node, which is on the reference path.
            
#ifdef debug
            cerr << "Reached anchoring node " << path.front().node_id() << endl;
            cerr << "Emit path of length " << path.size() << endl;
#endif
            
            // Say we got to the right place
            toReturn.emplace(bp_length(path), move(path));
            
            // Don't bother looking for extensions, we already got there.
        } else if (path.front().node_id() == 0 && !path.front().backward() &&
            index.by_id.count(path.front().snarl().start().node_id())) {
            // This visit is to a snarl, which is on the reference path on its
            // left end.
            
#ifdef debug
            cerr << "Reached start of anchoring snarl " << path.front().snarl() << endl;
#endif
            
            // Say we got to the right place
            toReturn.emplace(bp_length(path), move(path));
            
            // Don't bother looking for extensions, we already got there.
        } else if (path.front().node_id() == 0 && path.front().backward() &&
            index.by_id.count(path.front().snarl().end().node_id())) {
            // This visit is to a snarl in reverse, which is on the reference
            // path on its right end.
            
#ifdef debug
            cerr << "Reached end of anchoring snarl " << path.front().snarl() << endl;
#endif
            
            // Say we got to the right place
            toReturn.emplace(bp_length(path), move(path));
            
            // Don't bother looking for extensions, we already got there.
        } else if (path.size() <= max_depth) {
            // We haven't hit the reference path yet, but we also haven't hit
            // the max depth. Extend with all the possible extensions.
            
            // Look left, possibly entering child snarls
            vector<Visit> prevVisits = snarl_manager.visits_left(path.front(), augmented.graph, in_snarl);
            
#ifdef debug
            cerr << "Consider " << prevVisits.size() << " prev visits" << endl;
            for (Visit vis : prevVisits) {
                cerr << "\t" << vis << endl;
            }
#endif
            
            for (auto prevVisit : prevVisits) {
                // For each node we can get to
                
                if (prevVisit.node_id() != 0) {
                    // This is a visit to a node
                    
                    // Make sure the edge is real, since it can't be a back-to-
                    // back site
                    Edge* edge = augmented.graph.get_edge(to_right_side(prevVisit), to_left_side(path.front()));
                    assert(edge != NULL);
                
                    // Fetch the actual node
                    Node* prevNode = augmented.graph.get_node(prevVisit.node_id());
                    
                    if (augmented.has_supports() && 
                        (total(augmented.get_support(prevNode)) == 0 || total(augmented.get_support(edge)) == 0)) {
                        // We have no support at all for visiting this node by this
                        // edge (but we do have some read support data)
                        
#ifdef debug
                        cerr << "Reject " << prevNode->id() << " with no support" << endl;
#endif
                        
                        continue;
                    }
                } else {
                    // This is a visit to a child snarl
                    
                    // Look at the node we would leave the child snarl on
                    // That node can't be shared with a snarl we are already at.
                    Node* prevNode = augmented.graph.get_node(to_left_side(prevVisit).node);
                    
                    if (augmented.has_supports() && total(augmented.get_support(prevNode)) == 0) {
                        // We have no support at all for visiting the far node of this snarl
                        
#ifdef debug
                        cerr << "Reject " << prevVisit.snarl() << " with no support on far node" << endl;
#endif
                        
                        continue;
                    }
                    
                    // TODO: when snarls are not back-to-back, check the connecting edges
                }
                
                
                if (stopIfVisited && alreadyQueued.count(prevVisit)) {
                    // We already have a way to get here.
                    
#ifdef debug
                    cerr << "Reject " << prevVisit << " which is already seen" << endl;
#endif
                    
                    continue;
                }
                
                if (stillToExtend >= max_width) {
                    // Don't consider this extension because we already have too
                    // many. But since every time we go around the outer loop
                    // here we pop something off the queue to consider extending
                    // it, we'll always have at least one slot left to fill with
                    // an extension, so we'll at least keep exploring one path.

                
#ifdef debug
                    cerr << "Reject " << prevVisit << " because queue is full" << endl;
#endif

                    
                    continue;
                }
                
#ifdef debug
                cerr << "Accept " << prevVisit << endl;
#endif
            
                // Make a new path extended left with the node
                list<Visit> extended(path);
                extended.push_front(prevVisit);
                toExtend.emplace_back(move(extended));
                stillToExtend++;
                
                // Remember we found a way to this node, so we don't try and
                // visit it other ways.
                alreadyQueued.insert(prevVisit);
            }
        } else if (path.size() >= max_depth) {
#ifdef debug
            cerr << "Path has reached max depth! Aborting!" << endl;
#endif
        } else {
            // We should have handled all the possibilities.
            assert(false);
        }
        
    }
    
    return toReturn;
}

set<pair<size_t, list<Visit>>> RepresentativeTraversalFinder::bfs_right(Visit visit, PathIndex& index, bool stopIfVisited,
                                                                        const Snarl* in_snarl) {

    // Look left from the backward version of the visit.
    auto toConvert = bfs_left(reverse(visit), index, stopIfVisited, in_snarl);
    
    // Since we can't modify set records in place, we need to do a copy
    set<pair<size_t, list<Visit>>> toReturn;
    
    for(auto lengthAndPath : toConvert) {
        // Flip every path to run the other way
        lengthAndPath.second.reverse();
        for(auto& v : lengthAndPath.second) {
            // And invert the orientation of every visit in the path in place.
            v = reverse(v);
        }
        // Stick it in the new set
        toReturn.emplace(move(lengthAndPath));
    }
    
    return toReturn;
}

size_t RepresentativeTraversalFinder::bp_length(const list<Visit>& path) {
    size_t length = 0;
    for(auto& visit : path) {
        // Sum up length of each node's sequence
        if (visit.node_id() != 0) {
            length += augmented.graph.get_node(visit.node_id())->sequence().size();
        }
        // TODO: handle nested sites
    }
    return length;
}

Support make_support(double forward, double reverse, double quality) {
    Support to_return;
    to_return.set_forward(forward);
    to_return.set_reverse(reverse);
    to_return.set_quality(quality);
    return to_return;
}

double total(const Support& support) {
    return support.forward() + support.reverse();
}

Support support_min(const Support& a, const Support& b) {
    Support to_return;
    to_return.set_forward(min(a.forward(), b.forward()));
    to_return.set_reverse(min(a.reverse(), b.reverse()));
    to_return.set_quality(min(a.quality(), b.quality()));
    return to_return;
}

Support support_max(const Support& a, const Support& b) {
    Support to_return;
    to_return.set_forward(max(a.forward(), b.forward()));
    to_return.set_reverse(max(a.reverse(), b.reverse()));
    to_return.set_quality(max(a.quality(), b.quality()));    
    return to_return;
}

Support operator+(const Support& one, const Support& other) {
    Support sum;
    sum.set_forward(one.forward() + other.forward());
    sum.set_reverse(one.reverse() + other.reverse());
    sum.set_left(one.left() + other.left());
    sum.set_right(one.right() + other.right());
    
    // log-scaled quality can just be added
    sum.set_quality(one.quality() + other.quality());
    
    return sum;
}

Support& operator+=(Support& one, const Support& other) {
    one.set_forward(one.forward() + other.forward());
    one.set_reverse(one.reverse() + other.reverse());
    one.set_left(one.left() + other.left());
    one.set_right(one.right() + other.right());
    
    // log-scaled quality can just be added
    one.set_quality(one.quality() + other.quality());
    
    return one;
}

bool operator< (const Support& a, const Support& b) {
    return total(a) < total(b);
}

bool operator> (const Support& a, const Support& b) {
    return total(a) > total(b);
}

ostream& operator<<(ostream& stream, const Support& support) {
    return stream << support.forward() << "," << support.reverse();
}

string to_vcf_genotype(const Genotype& gt) {
    // Emit parts into this stream
    stringstream stream;
    
    for (size_t i = 0; i < gt.allele_size(); i++) {
        // For each allele called as present in the genotype
        
        // Put it in the string
        stream << gt.allele(i);
        
        if (i + 1 != gt.allele_size()) {
            // Write a separator after all but the last one
            stream << (gt.is_phased() ? '|' : '/');
        }
    }
    
    return stream.str();
}


}
