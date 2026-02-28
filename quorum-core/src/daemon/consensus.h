#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sui::quorum {

enum class ProposalState : uint8_t {
    Draft      = 0,
    Reviewing  = 1,
    Approved   = 2,
    Rejected   = 3,
    Escalated  = 4,
    Executed   = 5,
    Evaluated  = 6,
};

struct LocalProposal {
    std::string id;
    std::string title;
    std::string author;
    ProposalState state{ProposalState::Draft};
    uint64_t created_at{0};
    uint64_t updated_at{0};
    uint8_t current_round{0};
    std::vector<std::string> required_reviewers;
    std::vector<std::string> approvals;
    std::vector<std::string> rejections;
    std::string walrus_blob_id;
};

class ConsensusEngine {
public:
    static constexpr uint8_t MAX_ROUNDS = 3;

    [[nodiscard]] bool can_transition(ProposalState from, ProposalState to) const {
        switch (from) {
            case ProposalState::Draft:     return to == ProposalState::Reviewing;
            case ProposalState::Reviewing:
                return to == ProposalState::Approved ||
                       to == ProposalState::Rejected ||
                       to == ProposalState::Escalated;
            case ProposalState::Approved:  return to == ProposalState::Executed;
            case ProposalState::Executed:  return to == ProposalState::Evaluated;
            default: return false;
        }
    }

    [[nodiscard]] bool transition(LocalProposal& proposal, ProposalState to) {
        if (!can_transition(proposal.state, to)) return false;
        proposal.state = to;
        return true;
    }
};

} // namespace sui::quorum
