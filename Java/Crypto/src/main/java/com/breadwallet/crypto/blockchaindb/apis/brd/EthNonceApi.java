package com.breadwallet.crypto.blockchaindb.apis.brd;

import com.breadwallet.crypto.blockchaindb.BlockchainCompletionHandler;
import com.breadwallet.crypto.blockchaindb.errors.QueryError;
import com.google.common.base.Optional;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;

import org.json.JSONObject;

public class EthNonceApi {

    private final BrdApiClient client;

    public EthNonceApi(BrdApiClient client) {
        this.client = client;
    }

    public void getNonceAsEth(String networkName, String address, int rid,
                              BlockchainCompletionHandler<String> handler) {
        JSONObject json = new JSONObject(ImmutableMap.of(
                "jsonrpc", "2.0",
                "method", "eth_getTransactionCount",
                "params", ImmutableList.of(address, "latest"),
                "id", Integer.valueOf(rid)
        ));

        client.makeRequestJson(networkName, json, new StringCompletionHandler() {
            @Override
            public void handleData(Optional<String> result) {
                // TODO: Do we want default values?
                handler.handleData(result.or("118"));
            }

            @Override
            public void handleError(QueryError error) {
                handler.handleError(error);
            }
        });
    }
}