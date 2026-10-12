pub mod common {
  pub mod p {
    #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
    #[repr(transparent)]
    pub struct Color(pub i32);

    impl Color {
      pub const UNSPECIFIED: Color = Color(0);

      /// The matchable classification of this value.
      #[inline]
      #[must_use]
      pub fn kind(self) -> ColorKind {
        match self.0 {
          0 => ColorKind::Unspecified,
          other => ColorKind::Unknown(other),
        }
      }
    }

    #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
    #[non_exhaustive]
    pub enum ColorKind {
      Unspecified,
      Unknown(i32),
    }
  }
}

pub mod arena {
  pub mod p {
    pub struct Sub<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> Sub<'a> {
      pub fn x(&self) -> i32 {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Sub<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }

    pub struct Record<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> Record<'a> {
      pub fn payload(&self) -> Option<record::Payload<'a>> {
        todo!()
      }
      pub fn flag(&self) -> Option<record::Flag> {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Record<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }

    pub mod record {
      pub enum Payload<'a> {
        Text(::rapidproto::ProtoStr<'a>),
        Blob(&'a [u8]),
        Number(i32),
        Color(super::super::super::common::p::Color),
        Sub(&'a super::Sub<'a>),
      }

      pub enum Flag {
        On(bool),
        Code(i32),
      }
    }
  }
}
