//! Typed little-endian field accessors. The generated layout constants are values
//! of these types; the offsets come from `docs/spec/format.yaml`, not from here.
//!
//! Every accessor takes the record's bytes and returns `None` if the record is too
//! short, so a truncated record is an error the caller must handle, never a panic.

macro_rules! int_field {
    ($name:ident, $ty:ty, $len:literal) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        pub struct $name(pub usize);

        impl $name {
            pub const LEN: usize = $len;

            pub fn get(self, rec: &[u8]) -> Option<$ty> {
                let b = rec.get(self.0..self.0.checked_add($len)?)?;
                Some(<$ty>::from_le_bytes(b.try_into().ok()?))
            }

            pub fn put(self, rec: &mut [u8], v: $ty) {
                rec[self.0..self.0 + $len].copy_from_slice(&v.to_le_bytes());
            }
        }
    };
}

int_field!(U16, u16, 2);
int_field!(U32, u32, 4);
int_field!(U64, u64, 8);
int_field!(I64, i64, 8);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Bytes<const N: usize>(pub usize);

impl<const N: usize> Bytes<N> {
    pub const LEN: usize = N;

    pub fn get(self, rec: &[u8]) -> Option<[u8; N]> {
        rec.get(self.0..self.0.checked_add(N)?)?.try_into().ok()
    }

    pub fn put(self, rec: &mut [u8], v: &[u8; N]) {
        rec[self.0..self.0 + N].copy_from_slice(v);
    }
}
