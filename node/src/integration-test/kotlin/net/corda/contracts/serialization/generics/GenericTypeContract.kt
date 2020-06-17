package net.corda.contracts.serialization.generics

import net.corda.core.contracts.CommandData
import net.corda.core.contracts.Contract
import net.corda.core.contracts.ContractState
import net.corda.core.identity.AbstractParty
import net.corda.core.transactions.LedgerTransaction
import java.util.Optional

@Suppress("unused")
class GenericTypeContract : Contract {
    override fun verify(tx: LedgerTransaction) {
        val state = tx.outputsOfType<State>()
        require(state.isNotEmpty()) {
            "Requires at least one data state"
        }
    }

    @Suppress("CanBeParameter")
    class State(val owner: AbstractParty, val data: DataObject) : ContractState {
        override val participants: List<AbstractParty> = listOf(owner)

        @Override
        override fun toString(): String {
            return data.toString()
        }
    }

    class Purchase(val price: Optional<DataObject>) : CommandData
}