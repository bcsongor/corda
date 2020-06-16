package net.corda.bn.flows

import co.paralleluniverse.fibers.Suspendable
import net.corda.bn.flows.extensions.BNMemberAuth
import net.corda.bn.states.MembershipState
import net.corda.bn.states.MembershipStatus
import net.corda.core.contracts.Command
import net.corda.core.contracts.StateAndRef
import net.corda.core.flows.CollectSignaturesFlow
import net.corda.core.flows.FinalityFlow
import net.corda.core.flows.FlowException
import net.corda.core.flows.FlowLogic
import net.corda.core.flows.FlowSession
import net.corda.core.flows.ReceiveFinalityFlow
import net.corda.core.flows.ReceiveTransactionFlow
import net.corda.core.flows.SendTransactionFlow
import net.corda.core.flows.SignTransactionFlow
import net.corda.core.identity.Party
import net.corda.core.node.StatesToRecord
import net.corda.core.transactions.SignedTransaction
import net.corda.core.transactions.TransactionBuilder
import net.corda.core.utilities.unwrap

abstract class MembershipManagementFlow<T> : FlowLogic<T>() {

    @Suspendable
    protected fun authorise(networkId: String, databaseService: DatabaseService, authorisationMethod: (MembershipState) -> Boolean) {
        val ourMembership = databaseService.getMembership(networkId, ourIdentity)?.state?.data
                ?: throw FlowException("Receiver is not member of a business network")
        if (!ourMembership.isActive()) {
            throw FlowException("Receiver's membership is not active")
        }
        if (!authorisationMethod(ourMembership)) {
            throw FlowException("Receiver is not authorised to activate membership")
        }
    }

    @Suspendable
    protected fun collectSignaturesAndFinaliseTransaction(
            builder: TransactionBuilder,
            observerSessions: List<FlowSession>,
            signers: List<Party>
    ): SignedTransaction {
        // send info to observers whether they need to sign the transaction
        observerSessions.forEach { it.send(it.counterparty in signers) }

        val selfSignedTransaction = serviceHub.signInitialTransaction(builder)
        val signerSessions = observerSessions.filter { it.counterparty in signers }
        val allSignedTransaction = subFlow(CollectSignaturesFlow(selfSignedTransaction, signerSessions))

        return subFlow(FinalityFlow(allSignedTransaction, observerSessions, StatesToRecord.ALL_VISIBLE))
    }

    @Suspendable
    protected fun signAndReceiveFinalisedTransaction(session: FlowSession, commandCheck: (Command<*>) -> Unit) {
        val isSigner = session.receive<Boolean>().unwrap { it }
        val stx = if (isSigner) {
            val signResponder = object : SignTransactionFlow(session) {
                override fun checkTransaction(stx: SignedTransaction) {
                    val command = stx.tx.commands.single()
                    commandCheck(command)

                    stx.toLedgerTransaction(serviceHub, false).verify()
                }
            }
            subFlow(signResponder)
        } else null

        subFlow(ReceiveFinalityFlow(session, stx?.id, StatesToRecord.ALL_VISIBLE))
    }

    @Suspendable
    protected fun onboardMembershipSync(
            networkId: String,
            activatedMembership: MembershipState,
            authorisedMemberships: Set<StateAndRef<MembershipState>>,
            observerSessions: List<FlowSession>,
            auth: BNMemberAuth,
            databaseService: DatabaseService
    ) {
        val activatedMemberSession = observerSessions.single { it.counterparty == activatedMembership.identity }
        val pendingAndSuspendedMemberships =
                if (auth.run { canActivateMembership(activatedMembership) || canSuspendMembership(activatedMembership) || canRevokeMembership(activatedMembership) }) {
                    databaseService.getAllMembershipsWithStatus(networkId, MembershipStatus.PENDING, MembershipStatus.ACTIVE, MembershipStatus.SUSPENDED)
                } else emptyList()
        sendMemberships(authorisedMemberships + pendingAndSuspendedMemberships, observerSessions, activatedMemberSession)
    }

    @Suspendable
    private fun sendMemberships(
            memberships: Collection<StateAndRef<MembershipState>>,
            observerSessions: List<FlowSession>,
            destinationSession: FlowSession
    ) {
        val membershipsTransactions = memberships.map {
            serviceHub.validatedTransactions.getTransaction(it.ref.txhash)
                    ?: throw FlowException("Transaction for membership with ${it.state.data.linearId} ID doesn't exist")
        }
        observerSessions.forEach { it.send(if (it.counterparty == destinationSession.counterparty) membershipsTransactions.size else 0) }
        membershipsTransactions.forEach { subFlow(SendTransactionFlow(destinationSession, it)) }
    }

    @Suspendable
    protected fun receiveMemberships(session: FlowSession) {
        val txNumber = session.receive<Int>().unwrap { it }
        repeat(txNumber) {
            subFlow(ReceiveTransactionFlow(session, true, StatesToRecord.ALL_VISIBLE))
        }
    }
}