/// Binary row payload (positional wire format bytes).
pub type EncodedRow = Vec<u8>;

/// Key-value sidecar data attached to a published message.
pub type Attachments = std::collections::HashMap<String, Vec<u8>>;
