package net.corda.bn.flows.extensions

import net.corda.bn.states.MembershipState

interface BNMemberAuth {
    fun canRequestMembership(membership: MembershipState): Boolean
    fun canActivateMembership(membership: MembershipState): Boolean
    fun canSuspendMembership(membership: MembershipState): Boolean
    fun canRevokeMembership(membership: MembershipState): Boolean
}