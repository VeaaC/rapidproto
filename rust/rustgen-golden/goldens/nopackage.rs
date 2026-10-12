pub mod common {
  #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
  #[repr(transparent)]
  pub struct Status(pub i32);

  impl Status {
    pub const OK: Status = Status(0);
    pub const BAD: Status = Status(1);

    /// The matchable classification of this value.
    #[inline]
    #[must_use]
    pub fn kind(self) -> StatusKind {
      match self.0 {
        0 => StatusKind::Ok,
        1 => StatusKind::Bad,
        other => StatusKind::Unknown(other),
      }
    }
  }

  #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
  #[non_exhaustive]
  pub enum StatusKind {
    Ok,
    Bad,
    Unknown(i32),
  }
}

pub mod arena {
  pub struct Thing<'a> {
    _marker: ::core::marker::PhantomData<&'a ()>,
  }

  impl<'a> Thing<'a> {
    pub fn id(&self) -> i32 {
      todo!()
    }
    pub fn status(&self) -> super::common::Status {
      todo!()
    }
    pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Thing<'a>, ::rapidproto::DecodeError> {
      todo!()
    }
  }
}
