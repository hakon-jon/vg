#ifndef VG_DISTRIBUTIONS_HPP_INCLUDED
#define VG_DISTRIBUTIONS_HPP_INCLUDED

// distributions.hpp: contains functions for probability distributions used in genotyping.


#include <map>
#include <cmath>

#include "utility.hpp"

namespace vg {

using namespace std;

// We use this slightly nonstandard type for our math. We wrap it so it's easy
// to change later.
using real_t = long double;

/**
 * Calculate the natural log of the gamma function of the given argument.
 */
inline real_t gamma_ln(real_t x) {

    real_t cofactors[] = {76.18009173, 
        -86.50532033,
        24.01409822,
        -1.231739516,
        0.120858003E-2,
        -0.536382E-5};    

    real_t x1 = x - 1.0;
    real_t tmp = x1 + 5.5;
    tmp -= (x1 + 0.5) * log(tmp);
    real_t ser = 1.0;
    for (int j=0; j<=5; j++) {
        x1 += 1.0;
        ser += cofactors[j]/x1;
    }
    real_t y =  (-1.0 * tmp + log(2.50662827465 * ser));

    return y;
}

/**
 * Calculate the natural log of the factorial of the given integer. TODO:
 * replace with a cache or giant lookup table from Freebayes.
 */
inline real_t factorial_ln(int n) {
    if (n < 0) {
        return (long double)-1.0;
    }
    else if (n == 0) {
        return (long double)0.0;
    }
    else {
        return gamma_ln(n + 1.0);
    }
}

/**
 * Raise a log probability to a power
 */
inline real_t pow_ln(real_t m, int n) {
    return m * n;
}

/**
 * Compute the number of ways to select k items from a collection of n
 * distinguishable items, ignoring order. Returns the natural log of the
 * (integer) result.
 */
inline real_t choose_ln(int n, int k) {
    return factorial_ln(n) - (factorial_ln(k) + factorial_ln(n - k));
}

/**
 * Compute the number of ways to select k_1, k_2, ... k_i items into i buckets
 * from a collection of n distinguishable items, ignoring order. All of the
 * items have to go into the buckets, so all k_i must sum to n. To compute
 * choose you have to call this function with a 2-element vector, to represent
 * the chosen and not-chosen buckets. Returns the natural log of the (integer)
 * result.
 */
inline real_t multinomial_choose_ln(int n, vector<int> k) {
    // We use the product-of-binomial-coefficients approach from
    // <https://en.wikipedia.org/wiki/Multinomial_theorem#Multinomial_coefficients>
    real_t product_of_binomials_ln = 0;
    
    // We sum up the bucket sizes as we go
    int bucket_sum = 0;
    
    for (auto& bucket_size : k) {
        // Increment the size of what we choose from
        bucket_sum += bucket_size;
        // Choose this many
        product_of_binomials_ln += choose_ln(bucket_sum, bucket_size);
    }
    
    // Make sure they actually gave us a proper decomposition of the items into
    // buckets.
    assert(bucket_sum == n);
    
    return product_of_binomials_ln;
}

/**
 * Compute the log probability of a Poisson-distributed process: observed events
 * in an interval where expected events happen on average.
 */
inline real_t poisson_prob_ln(int observed, real_t expected) {
    return log(expected) * (real_t) observed - expected - factorial_ln(observed);
}

/**
 * Get the probability for sampling the counts in obs from a set of categories
 * weighted by the probabilities in probs. Works for both double and real_t
 * probabilities. Also works for binomials.
 */
template <typename ProbIn>
real_t multinomial_sampling_prob_ln(const vector<ProbIn>& probs, const vector<int>& obs) {
    vector<real_t> factorials;
    vector<real_t> probsPowObs;
    factorials.resize(obs.size());
    transform(obs.begin(), obs.end(), factorials.begin(), factorial_ln);
    typename vector<ProbIn>::const_iterator p = probs.begin();
    vector<int>::const_iterator o = obs.begin();
    for (; p != probs.end() && o != obs.end(); ++p, ++o) {
        probsPowObs.push_back(pow_ln(log(*p), *o));
    }
    // Use the collection sum defined in utility.hpp
    return factorial_ln(sum(obs)) - sum(factorials) + sum(probsPowObs);
}

/**
 * Compute the probability of having the given number of successes or fewer in
 * the given number of trials, with the given success probability. Returns the
 * resulting log probability.
 */
template <typename ProbIn>
real_t binomial_cmf_ln(ProbIn success_logprob, size_t trials, size_t successes) {
    // Compute log probabilities for all cases
    vector<real_t> case_logprobs;
    
    if(successes > trials) {
        return prob_to_logprob(0);
    }
    
    for(size_t considered_successes = 0; considered_successes <= successes; considered_successes++) {
        // For every number of successes up to this one, add in the probability.
        case_logprobs.push_back(choose_ln(trials, considered_successes) +
            success_logprob * considered_successes +
            logprob_invert(success_logprob) * (trials - considered_successes));
    }
    
    // Sum up all those per-case probabilities
    return logprob_sum(case_logprobs);
}

/**
 * Get the log probability for sampling the given value from a geometric
 * distribution with the given success log probability. The geometric
 * distribution is the distribution of the number of trials, with a given
 * success probability, required to observe a single success.
 */
template <typename ProbIn>
real_t geometric_sampling_prob_ln(ProbIn success_logprob, size_t trials) {
    return logprob_invert(success_logprob) * (trials - 1) + success_logprob;
}

/**
 * Given a split of items across a certain number of categories, as ints between
 * the two given bidirectional iterators, advance to the next split and return
 * true. If there is no next split, leave the collection unchanged and return
 * false.
 */
template<typename Iter>
bool advance_split(Iter start, Iter end) {
    if (start == end) {
        // Base case: we hit the end. No more possible splits.
        return false;
    } else {
        // Try advancing what comes after us.
        auto next = start;
        ++next;
        if (advance_split(next, end)) {
            // It worked.
            return true;
        }
        
        // If that didn't work, try moving an item from here to what comes after us.
        // We also need to reset what comes after us to its initial state of everything in the first category.
        // This is easy because we know everything in what comes after us has made its way to the end.
        if (*start != 0 && next != end) {
            // We have something to move
            *start--;
            *next++;
            
            // Do the reset so everything after us is in the first category
            // after us.
            auto next_to_last = end;
            --next_to_last;
            if (next_to_last != next) {
                *next += *next_to_last;
                *next_to_last = 0;
            }
            
            return true;
        }
        
        // If that didn't work, we're out of stuff to do.
        return false;
    }
}


/**
 * Get the log probability for sampling any actual set of category counts that
 * is consistent with the constraints specified by obs, using the per-category
 * probabilities defined in probs.
 *
 * Obs maps from a vector of per-category flags (called a "class") to a number
 * of items that might be in any of the flagged categories.
 *
 * For example, if there are two equally likely categories, and one item flagged
 * as potentially from either category, the probability of sampling a set of
 * category counts consistent with that constraint is 1. If instead there are
 * three equally likely categories, and one item flagged as potentially from two
 * of the three but not the third, the probability of sampling a set of category
 * counts consistent with that constraint is 2/3.
 */
template<typename ProbIn>
real_t multinomial_censored_sampling_prob_ln(const vector<ProbIn>& probs, const unordered_map<vector<bool>, int>& obs) {
    // We have a state. We advance this state until we can't anymore.
    //
    // The state is, for each ambiguity class, a vector of length equal to
    // the number of set bits in the valence, and sum equal to the number of
    // reads int he category.
    //
    // We start with all the reads in the first spot in each class, and
    // advance/reset until we have iterated over all combinations of category
    // assignments for all classes.
    unordered_map<vector<bool>, vector<int>> splits_by_class;
    
    // Prepare the state
    for (auto& kv : obs) {
        // For each class, find the vector we will use to describe its read-to-
        // category assignments.
        auto& class_state = splits_by_class[kv.first];
        
        for (auto& bit : kv.first) {
            // Allocate a spot for each set bit
            if (bit) {
                class_state.push_back(0);
            }
        }
        // Make sure all our classes have at least one category in them.
        // No magic reads from nowhere allowed.
        assert(!class_state.empty());
        
        // Drop all the reads in the first category
        class_state.front() = kv.second;
    }
    
    // Now we loop over all the combinations of class states using a stack thing.
    list<decltype(splits_by_class)::iterator> stack;
    
    // And we keep these multinomial coefficients up to date, describing the
    // number of ways each state on the stack can be realized
    list<real_t> atom_counts_ln;
    
    // And maintain this vector of category counts for the state we are in. We
    // incrementally update it so we aren't always looping over all the classes
    // to rebuild it.
    vector<int> category_counts(probs.size());
    
    // We have a function to add in the contribution of a class's state
    auto add_class_state = [&](const pair<vector<bool>, vector<int>>& class_state) {
        auto count_it = class_state.second.begin();
        for (size_t i = 0; i < category_counts.size(); i++) {
            // For each category
            if (class_state.first.at(i)) {
                // If this ambiguity class touches it
                
                assert(count_it != class_state.second.end());
                
                // Add in the state's count
                category_counts.at(i) += *count_it;
                
                // And consume that state entry
                ++count_it;
            }
        }
        assert(count_it == class_state.second.end());
    };
    
    // And a function to back it out again
    auto remove_class_state = [&](const pair<vector<bool>, vector<int>>& class_state) {
        auto count_it = class_state.second.begin();
        for (size_t i = 0; i < category_counts.size(); i++) {
            // For each category
            if (class_state.first.at(i)) {
                // If this ambiguity class touches it
                
                assert(count_it != class_state.second.end());
                
                // Back out the state's count
                category_counts.at(i) -= *count_it;
                
                // And consume that state entry
                ++count_it;
            }
        }
        assert(count_it == class_state.second.end());
    };
    
    for (auto it = splits_by_class.begin(); it != splits_by_class.end(); ++it) {
        // Populate the stack with everything
        stack.push_back(it);
        
        // Calculate first multinomial coefficients
        atom_counts_ln.push_back(multinomial_choose_ln(obs.at(it->first), it->second));
        
        // And make sure the category counts are up to date.
        add_class_state(*it);
    }
    
    do {
        // Emit the current state
        // TODO: add its likelihood scaled by its multinomials
        cerr << "Category counts:" << endl;
        for (auto& count : category_counts) {
            cerr << count << endl;
        }
        
        
        do {
            // See if we can advance what's at the bottom of the stack
            // First clear it out of the category counts.
            remove_class_state(*stack.back());
            if (advance_split(stack.back()->second.begin(), stack.back()->second.end())) {
                // We advanced it successfully.
                
                // Put it back in the category counts
                add_class_state(*stack.back());
                
                // Calculate its multinomial coefficient again
                atom_counts_ln.back() = multinomial_choose_ln(obs.at(stack.back()->first), stack.back()->second);
                
                // We finally found something to advance, so stop ascending the stack.
                break;
            } else {
                // Pop off the back of the stack.
                stack.pop_back();
                atom_counts_ln.pop_back();
                
                // Keep looking up
            }
        } while (!stack.empty());
        
        if (!stack.empty()) {
            // We found *something* to advance and haven't finished.
            
            // Now fill in the whole stack again with the first split for every category.
            auto it = stack.back();
            ++it;
            
            while (it != splits_by_class.end()) {
                // Reset the split to all 0s except for the first entry.
                for (auto& entry : it->second) {
                    entry = 0;
                }
                it->second.front() = obs.at(it->first);
            
                // Populate the stack with the next class
                stack.push_back(it);
                
                // Calculate multinomial coefficient
                atom_counts_ln.push_back(multinomial_choose_ln(obs.at(it->first), it->second));
                
                // And make sure the category counts are up to date.
                add_class_state(*it);
                
                // Look for the next class
                ++it;
            }
        }
        
        // Otherwise we have finished looping over everything and so we should leave the stack empty.
        
    } while (!stack.empty());
    
    // Now we return the total likelihood
    // TODO: calculate
    return 0;
}

}

#endif

















