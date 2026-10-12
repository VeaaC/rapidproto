pub mod common {
  pub mod p {
    pub mod outer {
      #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
      #[repr(transparent)]
      pub struct Kind(pub i32);

      impl Kind {
        pub const UNSPECIFIED: Kind = Kind(0);
        pub const A: Kind = Kind(1);

        /// The matchable classification of this value.
        #[inline]
        #[must_use]
        pub fn kind(self) -> KindKind {
          match self.0 {
            0 => KindKind::Unspecified,
            1 => KindKind::A,
            other => KindKind::Unknown(other),
          }
        }
      }

      #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
      #[non_exhaustive]
      pub enum KindKind {
        Unspecified,
        A,
        Unknown(i32),
      }
    }
  }
}

pub mod arena {
  pub mod p {
    pub struct Outer<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> Outer<'a> {
      pub fn inner(&self) -> Option<&'a outer::Inner<'a>> {
        todo!()
      }
      pub fn many(&self) -> &'a [outer::Inner<'a>] {
        todo!()
      }
      pub fn kind(&self) -> super::super::common::p::outer::Kind {
        todo!()
      }
      pub fn by_name(&self) -> ::rapidproto::MapView<'a, ::rapidproto::ProtoStr<'a>, outer::Inner<'a>> {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Outer<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }

    pub mod outer {
      pub struct Inner<'a> {
        _marker: ::core::marker::PhantomData<&'a ()>,
      }

      impl<'a> Inner<'a> {
        pub fn id(&self) -> i32 {
          todo!()
        }
        pub fn parent(&self) -> Option<&'a super::Outer<'a>> {
          todo!()
        }
        pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Inner<'a>, ::rapidproto::DecodeError> {
          todo!()
        }
      }
    }

    pub struct Top<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> Top<'a> {
      pub fn outer(&self) -> Option<&'a Outer<'a>> {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Top<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }
  }
}
