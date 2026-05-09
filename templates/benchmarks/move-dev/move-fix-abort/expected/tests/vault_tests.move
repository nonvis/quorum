#[test_only]
module vault::vault_tests;

use sui::test_scenario as ts;
use sui::coin;
use sui::sui::SUI;
use vault::vault;

const ALICE: address = @0xA11CE;

#[test]
fun test_deposit_then_withdraw() {
    let mut sc = ts::begin(ALICE);
    {
        vault::init_for_testing(sc.ctx());
    };
    sc.next_tx(ALICE);
    let mut v = sc.take_shared<vault::Vault>();
    {
        let c = coin::mint_for_testing<SUI>(100, sc.ctx());
        vault::deposit(&mut v, c, sc.ctx());
        assert!(vault::balance_of(&v, ALICE) == 100, 0);
    };
    sc.next_tx(ALICE);
    {
        vault::withdraw(&mut v, 60, sc.ctx());
        assert!(vault::balance_of(&v, ALICE) == 40, 1);
    };
    ts::return_shared(v);
    sc.end();
}

// TODO (regression test): a second deposit by the same sender should NOT abort.
// The benchmark task is to add this test and fix the underlying bug in deposit().
