package net.corda.bn.flows

import co.paralleluniverse.fibers.Suspendable
import net.corda.bn.contracts.MembershipContract
import net.corda.bn.states.MembershipStatus
import net.corda.core.contracts.UniqueIdentifier
import net.corda.core.flows.FlowException
import net.corda.core.flows.FlowSession
import net.corda.core.flows.InitiatedBy
import net.corda.core.flows.InitiatingFlow
import net.corda.core.flows.StartableByRPC
import net.corda.core.transactions.SignedTransaction
import net.corda.core.transactions.TransactionBuilder

@InitiatingFlow
@StartableByRPC
class ActivateMembershipFlow(private val membershipId: UniqueIdentifier) : MembershipManagementFlow<SignedTransaction>() {

    @Suspendable
    override fun call(): SignedTransaction {
        val databaseService = serviceHub.cordaService(DatabaseService::class.java)
        val membership = databaseService.getMembership(membershipId)
                ?: throw FlowException("Membership state with $membershipId linear ID doesn't exist")

        // check whether party is authorised to initiate flow
        val networkId = membership.state.data.networkId
        val auth = BNUtils.loadBNMemberAuth()
        authorise(networkId, databaseService) { auth.canActivateMembership(it) }

        // fetch observers and signers
        val authorisedMemberships = databaseService.getMembersAuthorisedToModifyMembership(networkId, auth).toSet()
        val observers = authorisedMemberships.map { it.state.data.identity }.toSet() + membership.state.data.identity - ourIdentity
        val signers = authorisedMemberships.filter { it.state.data.isActive() }.map { it.state.data.identity }

        // building transaction
        val outputMembership = membership.state.data.copy(status = MembershipStatus.ACTIVE, modified = serviceHub.clock.instant())
        val builder = TransactionBuilder(serviceHub.networkMapCache.notaryIdentities.first())
                .addInputState(membership)
                .addOutputState(outputMembership)
                .addCommand(MembershipContract.Commands.Activate(), signers.map { it.owningKey })
        builder.verify(serviceHub)

        // send info to observers whether they need to sign the transaction
        val observerSessions = observers.map { initiateFlow(it) }
        val finalisedTransaction = collectSignaturesAndFinaliseTransaction(builder, observerSessions, signers)

        // send authorised memberships to new activated or new suspended member (status moved from PENDING to ACTIVE/SUSPENDED)
        // also send all non revoked memberships (ones that can be modified) if new activated member is authorised to modify them
        if (membership.state.data.isPending()) {
            onboardMembershipSync(networkId, outputMembership, authorisedMemberships, observerSessions, auth, databaseService)
        }

        return finalisedTransaction
    }
}

@InitiatedBy(ActivateMembershipFlow::class)
class ActivateMembershipResponderFlow(private val session: FlowSession) : MembershipManagementFlow<Unit>() {

    @Suspendable
    override fun call() {
        signAndReceiveFinalisedTransaction(session) {
            if (it.value !is MembershipContract.Commands.Activate) {
                throw FlowException("Only Activate command is allowed")
            }
        }
        receiveMemberships(session)
    }
}