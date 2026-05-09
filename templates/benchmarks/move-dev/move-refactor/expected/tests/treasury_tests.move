#[test_only]
module treasury::treasury_tests {

    use sui::test_scenario as ts;
    use sui::coin;
    use sui::sui::SUI;
    use treasury::treasury;

    const ADMIN: address = @0xAD;

    #[test]
    fun test_init_then_deposits() {
        let scenario = ts::begin(ADMIN);
        // ... abbreviated. The agent's job in the refactor is to keep this
        // file behaving identically while modernizing idioms (let mut where
        // appropriate, method syntax where natural).
        ts::end(scenario);
    }
}
